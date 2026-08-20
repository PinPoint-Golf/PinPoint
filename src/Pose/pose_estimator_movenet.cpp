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

#if defined(HAVE_OPENCV) && defined(HAVE_MOVENET) && defined(HAVE_ONNXRUNTIME)

#include "pose_estimator_movenet.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include "pp_debug.h"
#include "pp_model_path.h"   // bundle-vs-bare-exe model lookup
#include "pp_profiler.h"
#include "pp_os_metrics.h"
#include "pp_gpu_metrics.h"
#include <QFile>
#include <QFileInfo>
#include <QLibrary>

#include <onnxruntime_cxx_api.h>
#include "ort_log.h"
#include <opencv2/imgproc.hpp>

#ifdef WITH_COREML
#  include <coreml_provider_factory.h>
#endif

// All ORT state lives here so onnxruntime_cxx_api.h is not pulled in via the header.
struct PoseEstimatorMoveNet::OrtState {
    Ort::Env            env     { ORT_LOGGING_LEVEL_WARNING, "MoveNet", ppOrtLogCallback, nullptr };
    Ort::SessionOptions opts;
    Ort::RunOptions     runOpts;
    Ort::AllocatorWithDefaultOptions allocator;
    std::unique_ptr<Ort::Session>    session;

    std::string inputName;
    std::string outputName;

    // Persistent nanosecond-resolution timer for interval tracking.
    QElapsedTimer wallTimer;

    int64_t modelBytes = 0;   // [seam] model-file size as an ONNX.Pose arena proxy
    int64_t gpuBytes   = 0;   // measured process-VRAM delta across session create (GPU.Pose)
};

// ---------------------------------------------------------------------------

PoseEstimatorMoveNet::PoseEstimatorMoveNet(QObject *parent)
    : PoseEstimatorBase(parent)
{}

PoseEstimatorMoveNet::~PoseEstimatorMoveNet()
{
    if (m_ort && m_ort->modelBytes > 0)
        PP_PROFILE_MEM_SUB("ONNX.Pose", m_ort->modelBytes);
    if (m_ort && m_ort->gpuBytes > 0)
        PP_PROFILE_MEM_SUB("GPU.Pose", m_ort->gpuBytes);
}

QString PoseEstimatorMoveNet::modelPath(ModelVariant v)
{
    const QString file = (v == ModelVariant::Thunder)
#ifdef HAVE_MOVENET_THUNDER
        ? QStringLiteral(MOVENET_THUNDER_FILE)
#else
        ? QStringLiteral("movenet_singlepose_thunder.onnx")
#endif
        : QStringLiteral(MOVENET_LIGHTNING_FILE);

    return pinpoint::modelFilePath(file, QStringLiteral("movenet"));
}

int PoseEstimatorMoveNet::inputSize(ModelVariant v)
{
    return (v == ModelVariant::Thunder) ? 256 : 192;
}

bool PoseEstimatorMoveNet::isVariantAvailable(ModelVariant v)
{
    return QFile::exists(modelPath(v));
}

void PoseEstimatorMoveNet::reloadModel(int variant)
{
    m_variant = (variant == static_cast<int>(ModelVariant::Thunder))
                ? ModelVariant::Thunder
                : ModelVariant::Lightning;
    load();
}

void PoseEstimatorMoveNet::load()
{
    // load() is the live pose thread's run-loop entry (connected to the thread's
    // started() signal) — register it with the resource profiler here.
    pinpoint::osmetrics::registerThread("Pose.Worker");

    const QString path = modelPath(m_variant);
    if (!QFile::exists(path)) {
        ppError() << "[MoveNet] Model not found:" << path;
        return;
    }

    m_ready = false;
    if (m_ort && m_ort->modelBytes > 0)   // reload: release the prior arena estimate
        PP_PROFILE_MEM_SUB("ONNX.Pose", m_ort->modelBytes);
    if (m_ort && m_ort->gpuBytes > 0)
        PP_PROFILE_MEM_SUB("GPU.Pose", m_ort->gpuBytes);
    m_ort   = std::make_unique<OrtState>();
    m_ort->opts.SetIntraOpNumThreads(1);
    m_ort->opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

    // Execution provider cascade — mirrors KokoroTTSEngine.
    QString epLabel;

#ifdef WITH_COREML
    if (epLabel.isEmpty()) {
        try {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                m_ort->opts, COREML_FLAG_USE_NONE));
            epLabel = QStringLiteral("CoreML");
            ppInfo() << "[MoveNet] CoreML execution provider active";
        } catch (const Ort::Exception &e) {
            ppInfo() << "[MoveNet] CoreML unavailable:" << e.what() << "— falling back";
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
                ppInfo() << "[MoveNet] CUDA execution provider active";
            } catch (const Ort::Exception &e) {
                ppInfo() << "[MoveNet] CUDA unavailable:" << e.what() << "— falling back";
            }
        }
    }
#endif

#ifdef WITH_DIRECTML
    if (epLabel.isEmpty()) {
        try {
            m_ort->opts.AppendExecutionProvider("DML");
            epLabel = QStringLiteral("DirectML");
            ppInfo() << "[MoveNet] DirectML execution provider active";
        } catch (const Ort::Exception &e) {
            ppInfo() << "[MoveNet] DirectML unavailable:" << e.what() << "— falling back";
        }
    }
#endif

    if (epLabel.isEmpty())
        ppInfo() << "[MoveNet] No GPU EP available — using CPU";
    else
        ppInfo() << "[MoveNet] Using" << epLabel << "execution provider";

    // Bracket the session build with a process-VRAM sample so we can attribute the
    // weights+arena (and, for the first GPU model loaded, the one-time CUDA/cuDNN
    // context) to GPU.Pose. Returns 0 on CPU/no-GPU hosts → no GPU.Pose category.
    const uint64_t gpuBefore = pinpoint::gpumetrics::processGpuBytes();
    try {
#ifdef Q_OS_WIN
        m_ort->session = std::make_unique<Ort::Session>(
            m_ort->env, path.toStdWString().c_str(), m_ort->opts);
#else
        m_ort->session = std::make_unique<Ort::Session>(
            m_ort->env, path.toUtf8().constData(), m_ort->opts);
#endif
    } catch (const Ort::Exception &e) {
        ppError() << "[MoveNet] Failed to load model:" << e.what();
        m_ort.reset();
        return;
    }

    // Query actual input/output names from the graph.
    m_ort->inputName  = m_ort->session->GetInputNameAllocated(0, m_ort->allocator).get();
    m_ort->outputName = m_ort->session->GetOutputNameAllocated(0, m_ort->allocator).get();

    m_ort->modelBytes = QFileInfo(path).size();   // [seam] file size as ORT arena proxy
    PP_PROFILE_MEM_ADD("ONNX.Pose", m_ort->modelBytes);

    const uint64_t gpuAfter = pinpoint::gpumetrics::processGpuBytes();
    if (gpuAfter > gpuBefore) {
        m_ort->gpuBytes = int64_t(gpuAfter - gpuBefore);
        PP_PROFILE_MEM_ADD("GPU.Pose", m_ort->gpuBytes);
    }

    const char *variantName = (m_variant == ModelVariant::Thunder) ? "Thunder" : "Lightning";
    const int   modelSz    = inputSize(m_variant);
    ppInfo() << "[MoveNet]" << variantName << "loaded —"
             << "input:" << m_ort->inputName.c_str()
             << "output:" << m_ort->outputName.c_str()
             << "size:" << modelSz << "×" << modelSz;

    m_ort->wallTimer.start();
    m_lastCallNs = -1;
    m_ready      = true;
    emit poseBackendReady(epLabel);
}

void PoseEstimatorMoveNet::estimatePose(const cv::Mat &frame)
{
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

    PP_PROFILE_SCOPE("Pose.MoveNet.run");   // full per-frame estimate (preprocess + infer + decode)

    // Record arrival time for FPS (interval between successive calls).
    const qint64 nowNs = m_ort->wallTimer.nsecsElapsed();

    const int sz = inputSize(m_variant);

    // Preprocess: resize → RGB → int32 [0, 255].
    // The Xenova/Transformers.js MoveNet ONNX export expects raw pixel values
    // as int32, not normalised float32.
    cv::Mat resized, rgb, rgb32;
    cv::resize(frame, resized, cv::Size(sz, sz));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb32, CV_32SC3);  // uint8 [0,255] → int32 [0,255], no scale

    // Inference.
    QElapsedTimer inferTimer;
    inferTimer.start();

    try {
        PP_PROFILE_SCOPE("Pose.MoveNet.infer");   // ORT Run() + keypoint decode

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::array<int64_t, 4> inputShape{ 1, sz, sz, 3 };
        auto inputTensor = Ort::Value::CreateTensor<int32_t>(
            memInfo,
            rgb32.ptr<int32_t>(), static_cast<size_t>(sz * sz * 3),
            inputShape.data(), inputShape.size());

        const char *inputNames[]  = { m_ort->inputName.c_str() };
        const char *outputNames[] = { m_ort->outputName.c_str() };
        Ort::Value  inputs[]      = { std::move(inputTensor) };

        auto outputs = m_ort->session->Run(
            m_ort->runOpts,
            inputNames, inputs, 1,
            outputNames, 1);

        // Parse [1, 1, 17, 3] → PoseResult.  Each keypoint is (y, x, score).
        const float *out = outputs[0].GetTensorData<float>();
        PoseResult result;
        result.timestamp = QDateTime::currentMSecsSinceEpoch();
        for (int j = 0; j < PoseResult::kNumKeypoints; ++j) {
            result.keypoints[j].y     = out[j * 3 + 0];
            result.keypoints[j].x     = out[j * 3 + 1];
            result.keypoints[j].score = out[j * 3 + 2];
        }
        float scoreSum = 0.f;
        for (const auto &kp : result.keypoints) scoreSum += kp.score;
        result.confidence = scoreSum / PoseResult::kNumKeypoints;

        emit poseEstimated(result);

    } catch (const Ort::Exception &e) {
        ppError() << "[MoveNet] Inference error:" << e.what();
        emit estimationDone();
        return;
    }

    const double inferMs   = inferTimer.nsecsElapsed() / 1e6;
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
        const double avgInferMs = m_inferenceSum / kWindowSize;
        const double avgInterval = m_intervalSum / kWindowSize;
        const double fps = (avgInterval > 0.0) ? 1000.0 / avgInterval : 0.0;
        emit poseStatsUpdated(avgInferMs, fps);
    }

    emit estimationDone();
}

#endif // HAVE_OPENCV && HAVE_MOVENET && HAVE_ONNXRUNTIME
