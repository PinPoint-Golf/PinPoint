#include "tts_controller.h"

#include "audio_output.h"
#include "ModelDownloader.h"
#include "TTSEngineFactory.h"
#include "TtsWorker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QMetaObject>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>

// ---------------------------------------------------------------------------
// Kokoro model download URLs
//
// Files are hosted on HuggingFace.  The direct-download URL format is:
//   https://huggingface.co/<owner>/<repo>/resolve/<branch>/<path>
//
// UPDATE THESE if the upstream repository moves the files.
// ---------------------------------------------------------------------------
static const char kBaseUrl[]   = "https://huggingface.co/onnx-community/Kokoro-82M-ONNX/resolve/main/";
static const char kModelUrl[]  = "https://huggingface.co/onnx-community/Kokoro-82M-ONNX/resolve/main/onnx/model_quantized.onnx";
static const char kTokensUrl[] = "https://huggingface.co/onnx-community/Kokoro-82M-ONNX/resolve/main/tokenizer.json";

// Voice style vectors available in onnx-community/Kokoro-82M-ONNX.
static const struct { const char *name; const char *file; } kVoiceUrls[] = {
    { "af",          "voices/af.bin"          },
    { "af_bella",    "voices/af_bella.bin"    },
    { "af_nicole",   "voices/af_nicole.bin"   },
    { "af_sarah",    "voices/af_sarah.bin"    },
    { "af_sky",      "voices/af_sky.bin"      },
    { "am_adam",     "voices/am_adam.bin"     },
    { "am_michael",  "voices/am_michael.bin"  },
    { "bf_emma",     "voices/bf_emma.bin"     },
    { "bf_isabella", "voices/bf_isabella.bin" },
    { "bm_george",   "voices/bm_george.bin"   },
    { "bm_lewis",    "voices/bm_lewis.bin"    },
};

// ---------------------------------------------------------------------------

TtsController::TtsController(QObject *parent)
    : QObject(parent)
    , m_workerThread(new QThread(this))
    , m_audioOutput(new AudioOutput(this))
    , m_downloader(new ModelDownloader(this))
{
    m_workerThread->setObjectName(QStringLiteral("TtsWorkerThread"));

    // ---- Engine + worker ----------------------------------------------------
    auto engine = TTSEngineFactory::create(TTSEngineFactory::Backend::Kokoro);
    m_worker    = new TtsWorker(engine.release());
    m_worker->moveToThread(m_workerThread);

    // ---- Audio output -------------------------------------------------------
    QAudioFormat ttsFormat;
    ttsFormat.setSampleRate(24000);
    ttsFormat.setChannelCount(1);
    ttsFormat.setSampleFormat(QAudioFormat::Float);
    m_audioOutput->setPreferredFormat(ttsFormat);
    m_audioOutput->start();

    connect(m_worker, &TtsWorker::audioReady,
            this, &TtsController::onAudioReady,
            Qt::QueuedConnection);

    // ---- Worker signals -----------------------------------------------------
    connect(m_worker, &TtsWorker::modelReady,         this, &TtsController::onModelReady);
    connect(m_worker, &TtsWorker::modelFailed,        this, &TtsController::onModelFailed);
    connect(m_worker, &TtsWorker::backendChanged,     this, &TtsController::onBackendChanged);
    connect(m_worker, &TtsWorker::synthesisStarted,   this, &TtsController::onSynthesisStarted);
    connect(m_worker, &TtsWorker::synthesisFinished,  this, &TtsController::onSynthesisFinished);
    connect(m_worker, &TtsWorker::errorOccurred,      this, &TtsController::onTtsError);

    // ---- Downloader signals -------------------------------------------------
    connect(m_downloader, &ModelDownloader::progress,
            this, &TtsController::onDownloadProgress);
    connect(m_downloader, &ModelDownloader::fileComplete,
            this, &TtsController::onDownloadFileComplete);
    connect(m_downloader, &ModelDownloader::finished,
            this, &TtsController::onDownloadFinished);
    connect(m_downloader, &ModelDownloader::failed,
            this, &TtsController::onDownloadFailed);

    // ---- Start worker thread ------------------------------------------------
    m_workerThread->start();

    // ---- Check model files, download if missing -----------------------------
    if (modelFilesExist()) {
        triggerModelLoad();
    } else {
        startDownload();
    }
}

TtsController::~TtsController()
{
    m_downloader->abort();

    if (m_workerThread->isRunning()) {
        QMetaObject::invokeMethod(m_worker, [this]() {
            m_worker->stop();
            m_worker->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);

        m_workerThread->quit();
        m_workerThread->wait();
    }
    delete m_worker;
    m_worker = nullptr;

    m_audioOutput->stop();
}

QStringList TtsController::voices() const
{
    return {
        QStringLiteral("af"),
        QStringLiteral("af_bella"),
        QStringLiteral("af_nicole"),
        QStringLiteral("af_sarah"),
        QStringLiteral("af_sky"),
        QStringLiteral("am_adam"),
        QStringLiteral("am_michael"),
        QStringLiteral("bf_emma"),
        QStringLiteral("bf_isabella"),
        QStringLiteral("bm_george"),
        QStringLiteral("bm_lewis"),
    };
}

void TtsController::setVoice(const QString &voice)
{
    if (m_voice == voice)
        return;
    m_voice = voice;
    emit voiceChanged();

    if (modelFilesExist())
        triggerModelLoad();
}

void TtsController::speak(const QString &text)
{
    if (text.trimmed().isEmpty() || !m_ttsReady || m_ttsActive)
        return;
    const QString t = text.trimmed();
    QMetaObject::invokeMethod(m_worker, [this, t]() {
        m_worker->synthesise(t);
    }, Qt::QueuedConnection);
}

void TtsController::stopSpeaking()
{
    QMetaObject::invokeMethod(m_worker, [this]() {
        m_worker->stop();
    }, Qt::QueuedConnection);
}

void TtsController::replayLastAudio()
{
    if (m_lastAudioCache.isEmpty() || m_ttsActive)
        return;
    m_audioOutput->writeAudio(m_lastAudioCache, m_lastAudioFormat);
}

// ── Worker slots ─────────────────────────────────────────────────────────────

void TtsController::onAudioReady(const QByteArray &data, const QAudioFormat &format)
{
    m_lastAudioFormat = format;
    m_lastAudioCache.append(data);
    m_audioOutput->writeAudio(data, format);
}

void TtsController::onModelReady()
{
    m_ttsReady = true;
    emit ttsReadyChanged();
}

void TtsController::onModelFailed(const QString &error)
{
    qWarning() << "[TTS] Model load failed:" << error;
    m_ttsReady = false;
    emit ttsReadyChanged();
}

void TtsController::onBackendChanged(const QString &backend)
{
    if (m_ttsBackend == backend)
        return;
    m_ttsBackend = backend;
    emit ttsBackendChanged();
}

void TtsController::onSynthesisStarted()
{
    const bool hadCache = !m_lastAudioCache.isEmpty();
    m_lastAudioCache.clear();
    m_ttsActive = true;
    emit ttsActiveChanged();
    if (hadCache)
        emit canReplayChanged();
}

void TtsController::onSynthesisFinished()
{
    m_ttsActive = false;
    emit ttsActiveChanged();
    if (!m_lastAudioCache.isEmpty())
        emit canReplayChanged();
}

void TtsController::onTtsError(const QString &message)
{
    qWarning() << "[TTS]" << message;
}

// ── Downloader slots ──────────────────────────────────────────────────────────

void TtsController::onDownloadProgress(int fileIndex, int fileCount,
                                        qint64 bytesReceived, qint64 bytesTotal)
{
    const qreal fileShare = 1.0 / fileCount;
    const qreal fileDone  = (bytesTotal > 0)
                            ? static_cast<qreal>(bytesReceived) / bytesTotal
                            : 0.0;
    m_downloadProgress = (fileIndex * fileShare) + (fileDone * fileShare);
    emit downloadProgressChanged();
}

void TtsController::onDownloadFileComplete(const QString &path)
{
    setDownloadStatus(tr("Downloaded %1").arg(QFileInfo(path).fileName()));
}

void TtsController::onDownloadFinished()
{
    m_downloading      = false;
    m_downloadProgress = 1.0;
    emit downloadingChanged();
    emit downloadProgressChanged();
    setDownloadStatus(tr("Download complete"));
    triggerModelLoad();
}

void TtsController::onDownloadFailed(const QString &error)
{
    m_downloading = false;
    emit downloadingChanged();
    setDownloadStatus(tr("Download failed: %1").arg(error));
    qWarning() << "[TTS] Download failed:" << error;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool TtsController::modelFilesExist() const
{
    return QFile::exists(resolveModelPath())
        && QFile::exists(resolveVoicePath())
        && QFile::exists(resolveTokensPath());
}

void TtsController::startDownload()
{
    const QString dir = modelDataDir();

    QList<ModelDownloader::Item> items;
    items.append({ QUrl(QString::fromLatin1(kModelUrl)),
                   dir + QStringLiteral("kokoro-v0_19-quantized.onnx") });
    items.append({ QUrl(QString::fromLatin1(kTokensUrl)),
                   dir + QStringLiteral("tokens.json") });

    for (const auto &v : kVoiceUrls) {
        items.append({ QUrl(QString::fromLatin1(kBaseUrl) + QString::fromLatin1(v.file)),
                       dir + QStringLiteral("voices/") + QString::fromLatin1(v.name) + QStringLiteral(".bin") });
    }

    m_downloading      = true;
    m_downloadProgress = 0.0;
    emit downloadingChanged();
    emit downloadProgressChanged();
    setDownloadStatus(tr("Downloading Kokoro model…"));

    m_downloader->download(items);
}

void TtsController::triggerModelLoad()
{
    m_ttsReady = false;
    emit ttsReadyChanged();

    const QString model  = resolveModelPath();
    const QString voice  = resolveVoicePath();
    const QString tokens = resolveTokensPath();
    QMetaObject::invokeMethod(m_worker, [this, model, voice, tokens]() {
        m_worker->loadModel(model, voice, tokens);
    }, Qt::QueuedConnection);
}

void TtsController::setDownloadStatus(const QString &status)
{
    m_downloadStatus = status;
    emit downloadStatusChanged();
}

// Model files are stored in the platform-standard app data directory so they
// survive across rebuilds and are not bundled inside the executable package.
//   Linux:   ~/.local/share/PinPoint/models/kokoro/
//   macOS:   ~/Library/Application Support/PinPoint/models/kokoro/
//   Windows: %APPDATA%/PinPoint/models/kokoro/

QString TtsController::modelDataDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/models/kokoro/");
}

QString TtsController::resolveModelPath() const
{
    const QString base = modelDataDir();
    for (const QString &name : {
             QStringLiteral("kokoro-v0_19-quantized.onnx"),
             QStringLiteral("kokoro-v0_19.onnx") }) {
        if (QFile::exists(base + name))
            return base + name;
    }
    return base + QStringLiteral("kokoro-v0_19-quantized.onnx");
}

QString TtsController::resolveVoicePath() const
{
    return modelDataDir() + QStringLiteral("voices/") + m_voice + QStringLiteral(".bin");
}

QString TtsController::resolveTokensPath() const
{
    return modelDataDir() + QStringLiteral("tokens.json");
}
