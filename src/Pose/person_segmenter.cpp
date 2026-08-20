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

#if defined(HAVE_OPENCV) && defined(HAVE_SEGMENTER) && defined(HAVE_ONNXRUNTIME)

#include "person_segmenter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include "pp_debug.h"
#include "pp_model_path.h"   // bundle-vs-bare-exe model lookup
#include "pp_profiler.h"
#include "pp_gpu_metrics.h"

#include <onnxruntime_cxx_api.h>
#include "ort_log.h"
#include <opencv2/imgproc.hpp>

// ---------------------------------------------------------------------------
// ORT state — same pimpl pattern as PoseEstimatorMoveNet
// ---------------------------------------------------------------------------

struct PersonSegmenter::OrtState {
    Ort::Env                     env  { ORT_LOGGING_LEVEL_WARNING, "Segmenter", ppOrtLogCallback, nullptr };
    Ort::SessionOptions          opts;
    Ort::AllocatorWithDefaultOptions alloc;
    std::unique_ptr<Ort::Session>    session;
    Ort::RunOptions              runOpts;

    std::string inputName;
    std::string outputName;

    int64_t modelBytes = 0;   // [seam] model-file size as an ONNX.Segmenter arena proxy
    int64_t gpuBytes   = 0;   // measured process-VRAM delta across session create (GPU.Segmenter)
};

// ---------------------------------------------------------------------------

PersonSegmenter::PersonSegmenter()  = default;

PersonSegmenter::~PersonSegmenter()
{
    if (m_ort && m_ort->modelBytes > 0)
        PP_PROFILE_MEM_SUB("ONNX.Segmenter", m_ort->modelBytes);
    if (m_ort && m_ort->gpuBytes > 0)
        PP_PROFILE_MEM_SUB("GPU.Segmenter", m_ort->gpuBytes);
}

QString PersonSegmenter::modelPath()
{
    return pinpoint::modelFilePath(QStringLiteral(SEGMENTER_MODEL_FILE), QStringLiteral("segmenter"));
}

bool PersonSegmenter::isAvailable()
{
    return QFile::exists(modelPath());
}

bool PersonSegmenter::load()
{
    const QString path = modelPath();
    if (!QFile::exists(path)) {
        ppError() << "[Segmenter] Model not found:" << path;
        return false;
    }

    m_ready = false;
    if (m_ort && m_ort->modelBytes > 0)   // reload: release the prior estimates
        PP_PROFILE_MEM_SUB("ONNX.Segmenter", m_ort->modelBytes);
    if (m_ort && m_ort->gpuBytes > 0)
        PP_PROFILE_MEM_SUB("GPU.Segmenter", m_ort->gpuBytes);
    m_ort   = std::make_unique<OrtState>();
    m_ort->opts.SetIntraOpNumThreads(2);
    m_ort->opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

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
        ppError() << "[Segmenter] Failed to load model:" << e.what();
        m_ort.reset();
        return false;
    }

    m_ort->modelBytes = QFileInfo(path).size();   // [seam] file size as ORT arena proxy
    PP_PROFILE_MEM_ADD("ONNX.Segmenter", m_ort->modelBytes);
    const uint64_t gpuAfter = pinpoint::gpumetrics::processGpuBytes();
    if (gpuAfter > gpuBefore) {
        m_ort->gpuBytes = int64_t(gpuAfter - gpuBefore);
        PP_PROFILE_MEM_ADD("GPU.Segmenter", m_ort->gpuBytes);
    }

    m_ort->inputName = m_ort->session->GetInputNameAllocated(0, m_ort->alloc).get();

    // u2netp has 7 outputs (d1…d6 + d0). "d0" is the final refined prediction.
    // Search for it explicitly; fall back to index 0 for other models.
    const size_t numOut = m_ort->session->GetOutputCount();
    int outputIdx = 0;
    for (size_t i = 0; i < numOut; ++i) {
        std::string name = m_ort->session->GetOutputNameAllocated(i, m_ort->alloc).get();
        if (name == "d0") { outputIdx = static_cast<int>(i); break; }
    }
    m_ort->outputName = m_ort->session->GetOutputNameAllocated(outputIdx, m_ort->alloc).get();

    // Determine input spatial size and layout (NHWC vs NCHW) from the graph.
    const auto inputInfo  = m_ort->session->GetInputTypeInfo(0);
    const auto inputShape = inputInfo.GetTensorTypeAndShapeInfo().GetShape();

    if (inputShape.size() == 4) {
        if (inputShape[3] == 3) {
            m_isNHWC = true;
            m_inputH = static_cast<int>(inputShape[1]);
            m_inputW = static_cast<int>(inputShape[2]);
        } else {
            m_isNHWC = false;
            m_inputH = static_cast<int>(inputShape[2]);
            m_inputW = static_cast<int>(inputShape[3]);
        }
    }

    if (m_inputH <= 0 || m_inputW <= 0) { m_inputH = 320; m_inputW = 320; }

    ppInfo() << "[Segmenter] Loaded — input:" << m_ort->inputName.c_str()
             << (m_isNHWC ? "NHWC" : "NCHW") << m_inputH << "×" << m_inputW
             << "output:" << m_ort->outputName.c_str();

    m_ready = true;
    return true;
}

cv::Mat PersonSegmenter::segment(const cv::Mat &bgr) const
{
    if (!m_ready || bgr.empty())
        return {};

    const int origH = bgr.rows;
    const int origW = bgr.cols;

    // Resize → BGR→RGB → float32 → ImageNet mean/std normalisation.
    // u2netp was trained with the standard ImageNet statistics.
    cv::Mat resized, rgb, rgbF;
    cv::resize(bgr, resized, cv::Size(m_inputW, m_inputH));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgbF, CV_32FC3, 1.0 / 255.0);

    {
        static const float mean[] = {0.485f, 0.456f, 0.406f};
        static const float std_d[] = {0.229f, 0.224f, 0.225f};
        std::vector<cv::Mat> chs(3);
        cv::split(rgbF, chs);
        for (int c = 0; c < 3; ++c)
            chs[c] = (chs[c] - mean[c]) / std_d[c];
        cv::merge(chs, rgbF);
    }

    // Build the input tensor.
    std::vector<float> inputData;
    std::vector<int64_t> inputShape;

    if (m_isNHWC) {
        // rgbF is already HWC — copy flat.
        inputShape = { 1, m_inputH, m_inputW, 3 };
        inputData.assign(rgbF.ptr<float>(),
                         rgbF.ptr<float>() + m_inputH * m_inputW * 3);
    } else {
        // Transpose to CHW.
        inputShape = { 1, 3, m_inputH, m_inputW };
        inputData.resize(static_cast<size_t>(3 * m_inputH * m_inputW));
        std::vector<cv::Mat> chans(3);
        cv::split(rgbF, chans);
        for (int c = 0; c < 3; ++c) {
            std::copy(chans[c].ptr<float>(),
                      chans[c].ptr<float>() + m_inputH * m_inputW,
                      inputData.data() + c * m_inputH * m_inputW);
        }
    }

    cv::Mat mask;
    try {
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        auto tensor = Ort::Value::CreateTensor<float>(
            memInfo,
            inputData.data(), inputData.size(),
            inputShape.data(), inputShape.size());

        const char *inNames[]  = { m_ort->inputName.c_str() };
        const char *outNames[] = { m_ort->outputName.c_str() };
        Ort::Value  inputs[]   = { std::move(tensor) };

        auto outputs = m_ort->session->Run(
            m_ort->runOpts, inNames, inputs, 1, outNames, 1);

        const auto &outInfo  = outputs[0].GetTensorTypeAndShapeInfo();
        const auto  outShape = outInfo.GetShape();
        const float *outPtr  = outputs[0].GetTensorData<float>();

        // Output is [N, H, W, 1] or [N, 1, H, W] — extract HW.
        int outH, outW;
        if (outShape.size() == 4 && outShape[3] == 1) {
            outH = static_cast<int>(outShape[1]);
            outW = static_cast<int>(outShape[2]);
        } else {
            outH = static_cast<int>(outShape[2]);
            outW = static_cast<int>(outShape[3]);
        }

        cv::Mat raw(outH, outW, CV_32F, const_cast<float *>(outPtr));

        // If values look like logits (not already sigmoid-activated), apply sigmoid.
        double minVal, maxVal;
        cv::minMaxLoc(raw, &minVal, &maxVal);
        if (maxVal > 1.5 || minVal < -0.5) {
            // logits — apply sigmoid
            cv::Mat sig;
            cv::exp(-raw, sig);
            mask = 1.0f / (1.0f + sig);
        } else {
            mask = raw.clone();
        }

        // Resize mask back to original frame size.
        cv::resize(mask, mask, cv::Size(origW, origH));

    } catch (const Ort::Exception &e) {
        ppError() << "[Segmenter] Inference error:" << e.what();
        return {};
    }

    return mask; // CV_32F, [0,1], person ≈ 1
}

#endif // HAVE_OPENCV && HAVE_SEGMENTER && HAVE_ONNXRUNTIME
