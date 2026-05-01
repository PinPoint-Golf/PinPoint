#include "KokoroTTSEngine.h"

#include <QDebug>
#include <QFile>

#ifdef HAVE_ONNXRUNTIME
#  include <onnxruntime_cxx_api.h>
#  ifdef ORT_COREML
#    include <coreml_provider_factory.h>
#  endif

// All ONNX Runtime state is isolated in OrtState so its headers are only
// pulled into this translation unit, not transitively via KokoroTTSEngine.h.
struct KokoroTTSEngine::OrtState {
    Ort::Env            env  { ORT_LOGGING_LEVEL_WARNING, "KokoroTTS" };
    Ort::SessionOptions opts;
    Ort::RunOptions     runOpts;
    std::unique_ptr<Ort::Session> session;
};
#endif

// ---------------------------------------------------------------------------

KokoroTTSEngine::KokoroTTSEngine(QObject *parent)
    : TTSEngine(parent)
{
#ifdef HAVE_ONNXRUNTIME
    m_ort = std::make_unique<OrtState>();
    m_ort->opts.SetIntraOpNumThreads(2);
    m_ort->opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

#if defined(ORT_COREML)
    try {
        // flags=0: let ORT decide which ops to offload to CoreML
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(m_ort->opts, 0));
        m_gpuBackend = QStringLiteral("CoreML");
    } catch (const Ort::Exception &e) {
        qDebug() << "KokoroTTS: CoreML unavailable:" << e.what() << "— using CPU";
    }
#elif defined(ORT_CUDA)
    // On Linux, probe for the NVIDIA kernel driver before asking ORT to load
    // the CUDA provider — avoids a verbose internal ORT error on machines with
    // no NVIDIA GPU (where the provider library loads but its CUDA deps don't).
#  ifdef Q_OS_LINUX
    if (!QFile::exists(QStringLiteral("/proc/driver/nvidia/version"))) {
        qDebug() << "KokoroTTS: No NVIDIA GPU detected — using CPU";
    } else
#  endif
    {
        try {
            OrtCUDAProviderOptions cuda{};
            cuda.device_id = 0;
            m_ort->opts.AppendExecutionProvider_CUDA(cuda);
            m_gpuBackend = QStringLiteral("CUDA");
        } catch (const Ort::Exception &e) {
            qDebug() << "KokoroTTS: CUDA unavailable:" << e.what() << "— using CPU";
        }
    }
#endif

    if (m_gpuBackend.isEmpty())
        qDebug() << "KokoroTTS: using CPU execution provider";
    else
        qDebug() << "KokoroTTS: using" << m_gpuBackend << "execution provider";
#endif
}

KokoroTTSEngine::~KokoroTTSEngine() = default;

bool KokoroTTSEngine::loadModel(const QString &modelPath,
                                 const QString &voicePath,
                                 const QString &tokensPath)
{
    m_ready = false;

#ifndef HAVE_ONNXRUNTIME
    emit errorOccurred(tr("Built without ONNX Runtime — KokoroTTSEngine non-functional"));
    return false;
#else
    // ---- Phoneme tokeniser --------------------------------------------------
    if (!m_tokenizer.initialise(tokensPath)) {
        emit errorOccurred(tr("PhonemeTokenizer: ") + m_tokenizer.lastError());
        return false;
    }

    // ---- Voice style vector (.bin = raw float32[256]) -----------------------
    QFile vf(voicePath);
    if (!vf.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("Cannot open voice file: ") + voicePath);
        return false;
    }
    const QByteArray vdata = vf.readAll();
    constexpr qint64 kExpected = 256 * static_cast<qint64>(sizeof(float));
    if (vdata.size() < kExpected) {
        emit errorOccurred(tr("Voice file too small (%1 bytes, expected %2)")
                               .arg(vdata.size()).arg(kExpected));
        return false;
    }
    m_styleVec.resize(256);
    std::memcpy(m_styleVec.data(), vdata.constData(), static_cast<size_t>(kExpected));

    // ---- ONNX model ---------------------------------------------------------
    try {
        m_ort->runOpts.UnsetTerminate();
#ifdef Q_OS_WIN
        m_ort->session = std::make_unique<Ort::Session>(
            m_ort->env, modelPath.toStdWString().c_str(), m_ort->opts);
#else
        m_ort->session = std::make_unique<Ort::Session>(
            m_ort->env, modelPath.toUtf8().constData(), m_ort->opts);
#endif
    } catch (const Ort::Exception &e) {
        emit errorOccurred(tr("ONNX load failed: %1")
                               .arg(QString::fromUtf8(e.what())));
        return false;
    }

    m_ready = true;
    emit modelLoaded();
    return true;
#endif // HAVE_ONNXRUNTIME
}

void KokoroTTSEngine::synthesise(const QString &text)
{
#ifndef HAVE_ONNXRUNTIME
    emit errorOccurred(tr("Built without ONNX Runtime"));
    return;
#else
    if (!m_ready) {
        emit errorOccurred(tr("Model not loaded"));
        return;
    }

    m_stopFlag.store(false);
    m_ort->runOpts.UnsetTerminate();
    emit synthesisStarted();

    // ---- Tokenise -----------------------------------------------------------
    const QVector<int64_t> tokens = m_tokenizer.tokenise(text);
    if (tokens.isEmpty()) {
        emit errorOccurred(tr("Tokenisation produced no tokens for input text"));
        emit synthesisFinished();
        return;
    }

    if (m_stopFlag.load()) { emit synthesisFinished(); return; }

    // ---- Run inference ------------------------------------------------------
    try {
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // tokens: [1, seq_len]
        std::vector<int64_t> tokenData(tokens.cbegin(), tokens.cend());
        std::array<int64_t, 2> tokenShape { 1, static_cast<int64_t>(tokenData.size()) };
        auto tokenTensor = Ort::Value::CreateTensor<int64_t>(
            memInfo,
            tokenData.data(), tokenData.size(),
            tokenShape.data(), tokenShape.size());

        // style: [1, 256]
        std::array<int64_t, 2> styleShape { 1, 256 };
        auto styleTensor = Ort::Value::CreateTensor<float>(
            memInfo,
            m_styleVec.data(), 256,
            styleShape.data(), styleShape.size());

        // speed: [1]
        float speedVal = m_speed;
        std::array<int64_t, 1> speedShape { 1 };
        auto speedTensor = Ort::Value::CreateTensor<float>(
            memInfo, &speedVal, 1,
            speedShape.data(), speedShape.size());

        if (m_stopFlag.load()) { emit synthesisFinished(); return; }

        const char *inputNames[]  = { "input_ids", "style", "speed" };
        const char *outputNames[] = { "waveform" };
        Ort::Value  inputs[]      = { std::move(tokenTensor),
                                      std::move(styleTensor),
                                      std::move(speedTensor) };

        // m_ort->runOpts.SetTerminate() can abort this call from stop().
        auto outputs = m_ort->session->Run(
            m_ort->runOpts,
            inputNames, inputs, 3,
            outputNames, 1);

        if (m_stopFlag.load()) { emit synthesisFinished(); return; }

        // ---- Package PCM output ---------------------------------------------
        const float   *samples    = outputs[0].GetTensorData<float>();
        const int64_t  numSamples =
            outputs[0].GetTensorTypeAndShapeInfo().GetShape().at(1);

        const QByteArray pcm(reinterpret_cast<const char *>(samples),
                             static_cast<int>(numSamples * sizeof(float)));
        emit audioReady(pcm, kokoroFormat());

    } catch (const Ort::Exception &e) {
        emit errorOccurred(tr("ONNX inference failed: %1")
                               .arg(QString::fromUtf8(e.what())));
    }

    emit synthesisFinished();
#endif // HAVE_ONNXRUNTIME
}

void KokoroTTSEngine::stop()
{
    m_stopFlag.store(true);
#ifdef HAVE_ONNXRUNTIME
    if (m_ort)
        m_ort->runOpts.SetTerminate();
#endif
}

bool KokoroTTSEngine::isReady() const
{
    return m_ready;
}

QAudioFormat KokoroTTSEngine::kokoroFormat()
{
    QAudioFormat fmt;
    fmt.setSampleRate(24000);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Float);
    return fmt;
}
