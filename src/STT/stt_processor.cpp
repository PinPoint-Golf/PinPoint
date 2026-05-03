#include "stt_processor.h"
#include "AudioConverter.h"
#include "STTBackendAzure.h"
#include "STTBackendFactory.h"
#include "STTWorker.h"
#include "SecretsManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <cmath>

Q_DECLARE_METATYPE(std::vector<float>)

STTProcessor::STTProcessor(QObject *parent)
    : AudioProcessorBase(parent)
    , m_flushTimer(new QTimer(this))
{
    qRegisterMetaType<std::vector<float>>();

    m_flushTimer->setInterval(m_chunkDurationMs);
    connect(m_flushTimer, &QTimer::timeout, this, &STTProcessor::onFlushTimer);

    auto backend = STTBackendFactory::createDefault();

    // If the best local backend can only use CPU, switch to Azure cloud transcription.
    // Check azureSttApiKey first; fall back to azureTtsApiKey so a single Azure
    // Cognitive Services resource covers both STT and TTS with one configured key.
    if (backend->backendLabel() == QLatin1String("CPU")) {
        QString apiKey = SecretsManager::read(QStringLiteral("azureSttApiKey"));
        if (apiKey.isEmpty())
            apiKey = SecretsManager::read(QStringLiteral("azureTtsApiKey"));
        if (!apiKey.isEmpty())
            backend = std::make_unique<STTBackendAzure>(apiKey);
    }

    m_needsModelFile = backend->requiresModelFile();
    m_silenceGating  = backend->requiresSilenceGating();
    m_worker = new STTWorker(backend.release());
    m_workerThread = new QThread;  // no parent — QThread cannot survive moveToThread recursion
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &STTWorker::transcriptionReady,
            this,     &STTProcessor::transcriptionReceived);
    connect(m_worker, &STTWorker::transcriptionFailed,
            this,     &STTProcessor::errorOccurred);
    connect(m_worker, &STTWorker::modelReady,
            this,     &STTProcessor::modelReady);
    connect(m_worker, &STTWorker::modelFailed,
            this,     &STTProcessor::errorOccurred);
    connect(m_worker, &STTWorker::backendLabelReady,
            this,     &STTProcessor::backendLabelReady);

    connect(m_workerThread, &QThread::finished,
            m_worker,       &QObject::deleteLater);

    m_workerThread->start();

    // Resolve the model path only for backends that require a local model file.
    if (m_needsModelFile) {
        m_modelPath = resolveModelPath(QStringLiteral("ggml-base.en.bin"));
        if (m_modelPath.isEmpty())
            m_searchedPaths = modelCandidates(QStringLiteral("ggml-base.en.bin"));
    }
    // Timer and model loading are started via start() after moveToThread().
}

STTProcessor::~STTProcessor()
{
    m_flushTimer->stop();
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
        delete m_workerThread;
        m_workerThread = nullptr;
    }
    // m_worker is deleted via the QThread::finished -> QObject::deleteLater connection.
}

void STTProcessor::start()
{
    if (m_needsModelFile && m_modelPath.isEmpty()) {
        emit modelNotFound(m_searchedPaths);
    } else {
        QMetaObject::invokeMethod(m_worker, "loadModel",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, m_modelPath));
    }
    m_flushTimer->start();
}

void STTProcessor::stopStreaming()
{
    // Discard any buffered audio so the flush timer cannot fire after this point
    // and call transcribe() on a closed socket, which would reopen the connection.
    m_buffer.clear();
    QMetaObject::invokeMethod(m_worker, "stopStreaming", Qt::QueuedConnection);
}

void STTProcessor::setChunkDurationMs(int ms)
{
    m_chunkDurationMs = ms;
    m_flushTimer->setInterval(ms);
}

void STTProcessor::processAudio(const QByteArray &data, const QAudioFormat &format)
{
    m_format = format;
    m_buffer.append(data);
}

void STTProcessor::onFlushTimer()
{
    if (m_buffer.isEmpty())
        return;

    if (m_silenceGating && computeRms(m_buffer, m_format) < m_silenceThreshold) {
        m_buffer.clear();
        return;
    }

    QByteArray chunk;
    chunk.swap(m_buffer);

    const std::vector<float> pcmF32 = AudioConverter::toWhisperFormat(
        chunk,
        m_format.sampleRate(),
        m_format.channelCount(),
        m_format.sampleFormat());
    if (!pcmF32.empty())
        QMetaObject::invokeMethod(m_worker, "transcribe",
                                  Qt::QueuedConnection,
                                  Q_ARG(std::vector<float>, pcmF32));
}

// Returns RMS amplitude normalised to 0.0–1.0.  Supports Int16 and Float formats;
// returns 1.0 (always send) for any other format so unknown formats are never gated.
double STTProcessor::computeRms(const QByteArray &pcm, const QAudioFormat &fmt) const
{
    if (pcm.isEmpty())
        return 0.0;

    double sum = 0.0;

    if (fmt.sampleFormat() == QAudioFormat::Int16) {
        const int count = pcm.size() / 2;
        if (count == 0) return 0.0;
        const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
        for (int i = 0; i < count; ++i) {
            const double s = samples[i] / 32768.0;
            sum += s * s;
        }
        return std::sqrt(sum / count);
    }

    if (fmt.sampleFormat() == QAudioFormat::Float) {
        const int count = pcm.size() / 4;
        if (count == 0) return 0.0;
        const auto *samples = reinterpret_cast<const float *>(pcm.constData());
        for (int i = 0; i < count; ++i) {
            const double s = samples[i];
            sum += s * s;
        }
        return std::sqrt(sum / count);
    }

    return 1.0;
}

// Returns the ordered list of candidate paths checked by resolveModelPath().
QStringList STTProcessor::modelCandidates(const QString &filename) const
{
    QStringList candidates;

    // 1. User/CI override via QSettings key "stt/modelPath"
    const QString settingsOverride =
        QSettings().value(QStringLiteral("stt/modelPath")).toString();
    if (!settingsOverride.isEmpty())
        candidates << settingsOverride;

    // 2. Platform app-data directory  (Linux: ~/.local/share/PinPoint/models/
    //                                   macOS: ~/Library/Application Support/PinPoint/models/
    //                                 Windows: %APPDATA%\PinPoint\models\)
    candidates << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/models/whisper/") + filename;

    // 3. models/ subfolder next to the executable (dev builds and portable installs)
    candidates << QCoreApplication::applicationDirPath()
                  + QStringLiteral("/models/whisper/") + filename;

    return candidates;
}

// Tries each candidate from modelCandidates() and returns the first path that
// exists as a file, or an empty string if none do.
QString STTProcessor::resolveModelPath(const QString &filename) const
{
    for (const QString &path : modelCandidates(filename)) {
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}
