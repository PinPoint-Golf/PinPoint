/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#if defined(HAVE_OPENCV) && defined(HAVE_VITPOSE) && defined(HAVE_ONNXRUNTIME)

#include "pose_estimator_vitpose.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QStandardPaths>
#include <algorithm>
#include <thread>
#include "pp_debug.h"
#include "pp_model_path.h"   // bundle-vs-bare-exe model lookup
#include "pp_profiler.h"
#include "cpu_topology.h"   // pinpoint::physicalCoreCount() (pose.intraOpThreads == -1)

#include <onnxruntime_cxx_api.h>
#include "ort_log.h"
#include <opencv2/imgproc.hpp>

#ifdef WITH_COREML
#  include <coreml_provider_factory.h>
#endif

// Model I/O constants.
static constexpr int kInputH      = 256;
static constexpr int kInputW      = 192;
static constexpr int kHeatmapH    = 64;
static constexpr int kHeatmapW    = 48;
static constexpr int kTotalJoints = 133; // COCO-WholeBody output channels
static constexpr int kBodyJoints  = 17;  // COCO body joints consumed here

// ImageNet normalisation constants (RGB order).
static constexpr float kMean[3] = { 0.485f, 0.456f, 0.406f };
static constexpr float kStd[3]  = { 0.229f, 0.224f, 0.225f };

// Decode one [kHeatmapH, kHeatmapW] channel per the active mode (heatmap_decode.h),
// normalised to [0, 1] in input space (the input image was resized to exactly
// kInputW × kInputH, so heatmap coords divide directly by the heatmap dims).
// Argmax is byte-identical to the original inline body-joint decode; Dark adds
// DARK sub-pixel refinement. Shared by the body and whole-body decode loops.
void PoseEstimatorViTPose::decodeChannel(const float *hm, float &nx, float &ny, float &score)
{
    if (m_decodeMode == DecodeMode::Dark)
        pinpoint::pose::decodeDark(hm, kHeatmapW, kHeatmapH, m_decodeBlur.data(), nx, ny, score);
    else
        pinpoint::pose::decodeArgmax(hm, kHeatmapW, kHeatmapH, nx, ny, score);
}

// All ORT state lives here so onnxruntime_cxx_api.h is not pulled in via the header.
struct PoseEstimatorViTPose::OrtState {
    Ort::Env            env     { ORT_LOGGING_LEVEL_WARNING, "ViTPose", ppOrtLogCallback, nullptr };
    Ort::SessionOptions opts;
    Ort::RunOptions     runOpts;
    Ort::AllocatorWithDefaultOptions allocator;
    std::unique_ptr<Ort::Session>    session;

    std::string inputName;
    std::string outputName;

    QElapsedTimer wallTimer;

    int64_t modelBytes = 0;   // [seam] model-file size as an ONNX.Pose arena proxy
};

// ---------------------------------------------------------------------------

// ViTPose++-L is never packaged (it is ~1.2 GB); it is downloaded on demand into
// the writable app-data dir. These coordinates are shared with the download
// controller (MotionCaptureProbe).
static constexpr char kLargeModelFile[] = "vitpose-l-wholebody.onnx";
static constexpr char kLargeModelUrl[]  =
    "https://huggingface.co/JunkyByte/easy_ViTPose/resolve/main/onnx/wholebody/"
    "vitpose-l-wholebody.onnx";

PoseEstimatorViTPose::PoseEstimatorViTPose(ModelVariant variant, QObject *parent)
    : PoseEstimatorBase(parent)
    , m_variant(variant)
{}

PoseEstimatorViTPose::~PoseEstimatorViTPose()
{
    if (m_ort && m_ort->modelBytes > 0)
        PP_PROFILE_MEM_SUB("ONNX.Pose", m_ort->modelBytes);
}

QString PoseEstimatorViTPose::largeModelFileName()
{
    return QString::fromLatin1(kLargeModelFile);
}

QString PoseEstimatorViTPose::largeModelDir()
{
    // Writable, survives rebuilds, never bundled (same convention as Kokoro TTS
    // voices in TtsController::modelDataDir()).
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
         + QStringLiteral("/models/vitpose/");
}

QString PoseEstimatorViTPose::largeModelUrl()
{
    return QString::fromLatin1(kLargeModelUrl);
}

QString PoseEstimatorViTPose::modelPath(ModelVariant v)
{
    if (v == ModelVariant::WholeBodyLarge)
        return largeModelDir() + QString::fromLatin1(kLargeModelFile);

    return pinpoint::modelFilePath(QStringLiteral(VITPOSE_MODEL_FILE), QStringLiteral("vitpose"));
}

bool PoseEstimatorViTPose::isVariantAvailable(ModelVariant v)
{
    return QFile::exists(modelPath(v));
}

void PoseEstimatorViTPose::load()
{
    const QString path = modelPath(m_variant);
    const char *variantTag = (m_variant == ModelVariant::WholeBodyLarge) ? "++-L" : "-B";
    if (!QFile::exists(path)) {
        ppError() << "[ViTPose" << variantTag << "] Model not found:" << path;
        return;
    }

    m_ready = false;
    if (m_ort && m_ort->modelBytes > 0)   // reload: release the prior arena estimate
        PP_PROFILE_MEM_SUB("ONNX.Pose", m_ort->modelBytes);
    m_ort   = std::make_unique<OrtState>();

    // ViTPose is used only on the offline analysis path (PoseRunner), and the
    // post-shot pipeline now sequences the x264 export AFTER the pose pass
    // (ShotProcessor::onAnalysisFinished) — so the estimator has the machine to
    // itself and should spread the inference across cores. This model is heavily
    // single-thread-bound otherwise: measured ~337 ms/frame at 1 intra-op thread
    // versus ~83 ms at the physical-core count on a 6-core/12-thread CPU
    // (hyperthreads past the physical-core count regress).
    //
    // Pool size resolves three ways (m_intraOpThreads, from pose.intraOpThreads —
    // PoseRunner sets it before load()):
    //   > 0   → pinned exactly (manual override)
    //   == -1 → topology auto: physical-core count clamped [1,16] via
    //           cpu_topology.h — OPT-IN; a determinism A/B on the affected hardware
    //           (no-SMT / hybrid P/E / >16-logical) is owed before it can be the
    //           default (docs/implementation/shaft_tracker_impl.md S0 note)
    //    0    → (DEFAULT) the legacy proxy: hardware_concurrency() reports logical
    //           cores, so halve it and clamp [1,8] — UNCHANGED, so the default path
    //           is thread-count-identical to the historical behaviour
    // (The live 60 Hz path uses MoveNet, which stays pinned to 1.)
    int intraThreads;
    if (m_intraOpThreads > 0) {
        intraThreads = m_intraOpThreads;
    } else if (m_intraOpThreads == -1) {
        intraThreads = std::clamp(pinpoint::physicalCoreCount(), 1, 16);
    } else {
        const unsigned hwThreads = std::thread::hardware_concurrency();
        intraThreads = std::clamp(static_cast<int>(hwThreads ? hwThreads / 2 : 1), 1, 8);
    }
    m_ort->opts.SetIntraOpNumThreads(intraThreads);
    m_ort->opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

    // Execution provider cascade — identical to PoseEstimatorMoveNet.
    QString epLabel;

#ifdef WITH_COREML
    if (epLabel.isEmpty()) {
        try {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                m_ort->opts, COREML_FLAG_USE_NONE));
            epLabel = QStringLiteral("CoreML");
            ppInfo() << "[ViTPose] CoreML execution provider active";
        } catch (const Ort::Exception &e) {
            ppInfo() << "[ViTPose] CoreML unavailable:" << e.what() << "— falling back";
        }
    }
#endif

#ifdef WITH_CUDA
    if (epLabel.isEmpty()) {
#  ifdef Q_OS_LINUX
        const bool hasNv = QFile::exists(QStringLiteral("/proc/driver/nvidia/version"));
#  elif defined(Q_OS_WIN)
        QLibrary nvcuda(QStringLiteral("nvcuda.dll"));
        const bool hasNv = nvcuda.load();
        if (hasNv) nvcuda.unload();
#  else
        const bool hasNv = false;
#  endif
        if (hasNv) {
            try {
                OrtCUDAProviderOptions cuda{};
                cuda.device_id = 0;
                m_ort->opts.AppendExecutionProvider_CUDA(cuda);
                epLabel = QStringLiteral("CUDA");
                ppInfo() << "[ViTPose] CUDA execution provider active";
            } catch (const Ort::Exception &e) {
                ppInfo() << "[ViTPose] CUDA unavailable:" << e.what() << "— falling back";
            }
        }
    }
#endif

#ifdef WITH_DIRECTML
    if (epLabel.isEmpty()) {
        try {
            m_ort->opts.AppendExecutionProvider("DML");
            epLabel = QStringLiteral("DirectML");
            ppInfo() << "[ViTPose] DirectML execution provider active";
        } catch (const Ort::Exception &e) {
            ppInfo() << "[ViTPose] DirectML unavailable:" << e.what() << "— falling back";
        }
    }
#endif

    if (epLabel.isEmpty())
        ppInfo() << "[ViTPose] No GPU EP available — using CPU";
    else
        ppInfo() << "[ViTPose] Using" << epLabel << "execution provider";

    try {
#ifdef Q_OS_WIN
        m_ort->session = std::make_unique<Ort::Session>(
            m_ort->env, path.toStdWString().c_str(), m_ort->opts);
#else
        m_ort->session = std::make_unique<Ort::Session>(
            m_ort->env, path.toUtf8().constData(), m_ort->opts);
#endif
    } catch (const Ort::Exception &e) {
        ppError() << "[ViTPose] Failed to load model:" << e.what();
        m_ort.reset();
        return;
    }

    m_ort->inputName  = m_ort->session->GetInputNameAllocated(0, m_ort->allocator).get();
    m_ort->outputName = m_ort->session->GetOutputNameAllocated(0, m_ort->allocator).get();

    m_ort->modelBytes = QFileInfo(path).size();   // [seam] file size as ORT arena proxy
    PP_PROFILE_MEM_ADD("ONNX.Pose", m_ort->modelBytes);

    ppInfo() << "[ViTPose" << variantTag << "] Loaded —" << QFileInfo(path).fileName()
             << "input:" << m_ort->inputName.c_str()
             << "output:" << m_ort->outputName.c_str()
             << "size:" << kInputW << "×" << kInputH
             << "intraOpThreads:" << intraThreads;

    m_decodeBlur.assign(size_t(kHeatmapH) * kHeatmapW, 0.f);   // DARK scratch

    m_ort->wallTimer.start();
    m_lastCallNs = -1;
    m_ready      = true;
    emit poseBackendReady(epLabel);
}

void PoseEstimatorViTPose::estimatePose(const cv::Mat &frame)
{
    // Whole-body keypoints from a previous frame must never outlive the call
    // that produced them — invalidate before any early-out. No-op live.
    if (m_decodeWholeBody)
        m_lastWholeBody.valid = false;

    if (!isEnabled()) {     // disabled by method — release the throttle, skip inference
        emit estimationDone();
        return;
    }

    if (!m_ready || frame.empty()) {
        // Model missing/failed to load, or a degenerate frame — still release
        // the throttle, or the whole camera pipeline (pose AND ball) starves
        // permanently on the first frame.
        emit estimationDone();
        return;
    }

    PP_PROFILE_SCOPE("Pose.ViTPose.run");   // full per-frame estimate (preprocess + infer + decode)

    const qint64 nowNs = m_ort->wallTimer.nsecsElapsed();

    // Preprocess: resize → BGR→RGB → float32 [0,1] → ImageNet normalise → CHW.
    cv::Mat resized, rgb, rgbF;
    cv::resize(frame, resized, cv::Size(kInputW, kInputH));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgbF, CV_32FC3, 1.0 / 255.0);

    // Split into channel planes and normalise each channel.
    std::vector<cv::Mat> planes(3);
    cv::split(rgbF, planes);
    for (int c = 0; c < 3; ++c)
        planes[c] = (planes[c] - kMean[c]) / kStd[c];

    // Build contiguous NCHW tensor buffer [1, 3, 256, 192].
    static constexpr size_t kPlaneSize = kInputH * kInputW;
    std::vector<float> inputBuf(3 * kPlaneSize);
    for (int c = 0; c < 3; ++c) {
        cv::Mat dst(kInputH, kInputW, CV_32FC1, inputBuf.data() + c * kPlaneSize);
        planes[c].copyTo(dst);
    }

    QElapsedTimer inferTimer;
    inferTimer.start();

    try {
        PP_PROFILE_SCOPE("Pose.ViTPose.infer");   // ORT Run() + heatmap decode

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::array<int64_t, 4> inputShape{ 1, 3, kInputH, kInputW };
        auto inputTensor = Ort::Value::CreateTensor<float>(
            memInfo,
            inputBuf.data(), inputBuf.size(),
            inputShape.data(), inputShape.size());

        const char *inputNames[]  = { m_ort->inputName.c_str() };
        const char *outputNames[] = { m_ort->outputName.c_str() };
        Ort::Value  inputs[]      = { std::move(inputTensor) };

        auto outputs = m_ort->session->Run(
            m_ort->runOpts,
            inputNames, inputs, 1,
            outputNames, 1);

        // Output: [1, 133, 64, 48] — consume the first kBodyJoints (0–16)
        // channels always, plus ALL remaining feet/face/hand channels
        // (17–132) when whole-body decode is opted in.
        const float *heatmapData = outputs[0].GetTensorData<float>();

        PoseResult result;
        result.timestamp = QDateTime::currentMSecsSinceEpoch();

        for (int j = 0; j < kBodyJoints; ++j) {
            const float *hm = heatmapData + j * kHeatmapH * kHeatmapW;
            decodeChannel(hm, result.keypoints[j].x,
                                 result.keypoints[j].y, result.keypoints[j].score);
        }

        float scoreSum = 0.f;
        for (int j = 0; j < kBodyJoints; ++j) scoreSum += result.keypoints[j].score;
        result.confidence = scoreSum / kBodyJoints;

        // COCO-WholeBody tail channels — same decode pass, opt-in only (the
        // live 60 Hz path never pays for this). The body slots (0–16) are
        // COPIED from the PoseResult decode above — never re-decoded — so the
        // emitted PoseResult and WholeBodyResult.kp[0..16] are bit-identical.
        if (m_decodeWholeBody) {
            WholeBodyResult &wb = m_lastWholeBody;
            for (int j = 0; j < kBodyJoints; ++j) {
                wb.kp[size_t(j)]    = QPointF(result.keypoints[j].x, result.keypoints[j].y);
                wb.score[size_t(j)] = result.keypoints[j].score;
            }
            for (int j = kBodyJoints; j < kTotalJoints; ++j) {   // feet + face + hands
                const float *hm = heatmapData + j * kHeatmapH * kHeatmapW;
                float nx = 0.f, ny = 0.f, score = 0.f;
                decodeChannel(hm, nx, ny, score);
                wb.kp[size_t(j)]    = QPointF(nx, ny);
                wb.score[size_t(j)] = score;
            }
            wb.valid = true;
        }

        emit poseEstimated(result);

    } catch (const Ort::Exception &e) {
        ppError() << "[ViTPose] Inference error:" << e.what();
        emit estimationDone();
        return;
    }

    const double inferMs    = inferTimer.nsecsElapsed() / 1e6;
    const double intervalMs = (m_lastCallNs >= 0)
                              ? (nowNs - m_lastCallNs) / 1e6
                              : inferMs;
    m_lastCallNs = nowNs;

    // Update rolling circular buffers — O(1) per frame.
    m_inferenceSum -= m_inferenceSamples[m_timingIndex];
    m_intervalSum  -= m_intervalSamples[m_timingIndex];
    m_inferenceSamples[m_timingIndex] = inferMs;
    m_intervalSamples[m_timingIndex]  = intervalMs;
    m_inferenceSum += inferMs;
    m_intervalSum  += intervalMs;
    m_timingIndex   = (m_timingIndex + 1) % kWindowSize;
    if (m_timingCount < kWindowSize)
        ++m_timingCount;

    if (m_timingCount == kWindowSize) {
        const double avgInferMs  = m_inferenceSum / kWindowSize;
        const double avgInterval = m_intervalSum  / kWindowSize;
        const double fps = (avgInterval > 0.0) ? 1000.0 / avgInterval : 0.0;
        emit poseStatsUpdated(avgInferMs, fps);
    }

    emit estimationDone();
}

// Preprocess half of estimatePose() — identical math (lines above), factored so
// PoseRunner can run it on a producer thread. Touches no member state; the
// output `inputBuf` is fully self-owned (independent of `frame`'s pixels).
void PoseEstimatorViTPose::preprocess(const cv::Mat &frame, std::vector<float> &inputBuf) const
{
    // Preprocess: resize → BGR→RGB → float32 [0,1] → ImageNet normalise → CHW.
    cv::Mat resized, rgb, rgbF;
    cv::resize(frame, resized, cv::Size(kInputW, kInputH));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgbF, CV_32FC3, 1.0 / 255.0);

    // Split into channel planes and normalise each channel.
    std::vector<cv::Mat> planes(3);
    cv::split(rgbF, planes);
    for (int c = 0; c < 3; ++c)
        planes[c] = (planes[c] - kMean[c]) / kStd[c];

    // Build contiguous NCHW tensor buffer [1, 3, 256, 192].
    static constexpr size_t kPlaneSize = kInputH * kInputW;
    inputBuf.resize(3 * kPlaneSize);
    for (int c = 0; c < 3; ++c) {
        cv::Mat dst(kInputH, kInputW, CV_32FC1, inputBuf.data() + c * kPlaneSize);
        planes[c].copyTo(dst);
    }
}

// Inference half of estimatePose() — ORT Run() + heatmap decode + emit, over a
// buffer already produced by preprocess(). Caller guarantees the estimator is
// ready. `inputBuf` is not modified (ORT's CreateTensor wants a non-const
// pointer; the buffer is only read during Run()).
void PoseEstimatorViTPose::inferPrepared(std::vector<float> &inputBuf)
{
    // Whole-body keypoints from a previous frame must never outlive the call
    // that produced them.
    if (m_decodeWholeBody)
        m_lastWholeBody.valid = false;

    if (!m_ready) {
        emit estimationDone();
        return;
    }

    PP_PROFILE_SCOPE("Pose.ViTPose.run");

    const qint64 nowNs = m_ort->wallTimer.nsecsElapsed();

    QElapsedTimer inferTimer;
    inferTimer.start();

    try {
        PP_PROFILE_SCOPE("Pose.ViTPose.infer");   // ORT Run() + heatmap decode

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::array<int64_t, 4> inputShape{ 1, 3, kInputH, kInputW };
        auto inputTensor = Ort::Value::CreateTensor<float>(
            memInfo,
            inputBuf.data(), inputBuf.size(),
            inputShape.data(), inputShape.size());

        const char *inputNames[]  = { m_ort->inputName.c_str() };
        const char *outputNames[] = { m_ort->outputName.c_str() };
        Ort::Value  inputs[]      = { std::move(inputTensor) };

        auto outputs = m_ort->session->Run(
            m_ort->runOpts,
            inputNames, inputs, 1,
            outputNames, 1);

        const float *heatmapData = outputs[0].GetTensorData<float>();

        PoseResult result;
        result.timestamp = QDateTime::currentMSecsSinceEpoch();

        for (int j = 0; j < kBodyJoints; ++j) {
            const float *hm = heatmapData + j * kHeatmapH * kHeatmapW;
            decodeChannel(hm, result.keypoints[j].x,
                                 result.keypoints[j].y, result.keypoints[j].score);
        }

        float scoreSum = 0.f;
        for (int j = 0; j < kBodyJoints; ++j) scoreSum += result.keypoints[j].score;
        result.confidence = scoreSum / kBodyJoints;

        // Body slots copied from the PoseResult decode (bit-identical), tail
        // channels decoded fresh — mirrors estimatePose() exactly.
        if (m_decodeWholeBody) {
            WholeBodyResult &wb = m_lastWholeBody;
            for (int j = 0; j < kBodyJoints; ++j) {
                wb.kp[size_t(j)]    = QPointF(result.keypoints[j].x, result.keypoints[j].y);
                wb.score[size_t(j)] = result.keypoints[j].score;
            }
            for (int j = kBodyJoints; j < kTotalJoints; ++j) {   // feet + face + hands
                const float *hm = heatmapData + j * kHeatmapH * kHeatmapW;
                float nx = 0.f, ny = 0.f, score = 0.f;
                decodeChannel(hm, nx, ny, score);
                wb.kp[size_t(j)]    = QPointF(nx, ny);
                wb.score[size_t(j)] = score;
            }
            wb.valid = true;
        }

        emit poseEstimated(result);

    } catch (const Ort::Exception &e) {
        ppError() << "[ViTPose] Inference error:" << e.what();
        emit estimationDone();
        return;
    }

    const double inferMs    = inferTimer.nsecsElapsed() / 1e6;
    const double intervalMs = (m_lastCallNs >= 0)
                              ? (nowNs - m_lastCallNs) / 1e6
                              : inferMs;
    m_lastCallNs = nowNs;

    m_inferenceSum -= m_inferenceSamples[m_timingIndex];
    m_intervalSum  -= m_intervalSamples[m_timingIndex];
    m_inferenceSamples[m_timingIndex] = inferMs;
    m_intervalSamples[m_timingIndex]  = intervalMs;
    m_inferenceSum += inferMs;
    m_intervalSum  += intervalMs;
    m_timingIndex   = (m_timingIndex + 1) % kWindowSize;
    if (m_timingCount < kWindowSize)
        ++m_timingCount;

    if (m_timingCount == kWindowSize) {
        const double avgInferMs  = m_inferenceSum / kWindowSize;
        const double avgInterval = m_intervalSum  / kWindowSize;
        const double fps = (avgInterval > 0.0) ? 1000.0 / avgInterval : 0.0;
        emit poseStatsUpdated(avgInferMs, fps);
    }

    emit estimationDone();
}

#endif // HAVE_OPENCV && HAVE_VITPOSE && HAVE_ONNXRUNTIME
