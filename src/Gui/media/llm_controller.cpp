/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "llm_controller.h"

#include "app_settings.h"
#include "LlmEngine.h"
#include "LocalLlmEngine.h"
#include "GeminiLlmEngine.h"
#include "LlmWorker.h"
#include "ModelDownloader.h"
#include "SecretsManager.h"
#include "pp_debug.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

// Golf coach system prompt. Keep it brief — the model receives this on every
// chat() call so shorter = less context overhead.
static constexpr char kCoachSystemPrompt[] =
    "You are an expert golf coach providing real-time guidance during a training session. "
    "You lead the conversation with concise, encouraging coaching cues. "
    "Keep responses to 2-3 sentences — the golfer may be standing at the tee.";

// Phi-4-mini-instruct ONNX model on HuggingFace.
// Subdirectory within the repo: cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4/
static constexpr char kHfBase[] =
    "https://huggingface.co/microsoft/Phi-4-mini-instruct-onnx/resolve/main/"
    "cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4/";

// All files required by ORT-GenAI from that directory (~4.9 GB total).
static const char * const kModelFiles[] = {
    "genai_config.json",
    "model.onnx",
    "model.onnx.data",
    "tokenizer.json",
    "tokenizer_config.json",
    "special_tokens_map.json",
    "added_tokens.json",
    "merges.txt",
    "vocab.json",
    "config.json",
    "configuration_phi3.py",
};

// ---------------------------------------------------------------------------

LlmController::LlmController(AppSettings *settings, QObject *parent)
    : QObject(parent)
    , m_appSettings(settings)
    , m_workerThread(new QThread(this))
    , m_worker(nullptr)
    , m_downloader(new ModelDownloader(this))
{
    connect(m_downloader, &ModelDownloader::progress,
            this, &LlmController::onDownloadProgress);
    connect(m_downloader, &ModelDownloader::fileComplete,
            this, &LlmController::onDownloadFileComplete);
    connect(m_downloader, &ModelDownloader::finished,
            this, &LlmController::onDownloadFinished);
    connect(m_downloader, &ModelDownloader::failed,
            this, &LlmController::onDownloadFailed);

    const QString geminiKey = SecretsManager::read(QStringLiteral("geminiApiKey"));
    m_cloudToggleAvailable  = !geminiKey.isEmpty();
    m_localGpuAvailable     = LocalLlmEngine::hasGpu();
    if (m_appSettings)
        connect(m_appSettings, &AppSettings::cloudFallbackLlmChanged,
                this, &LlmController::applyCloudFallbackPref);

    // Cloud is used when the user forces it (cloudFallbackLlm) OR there is no local
    // GPU (the AI Coach has no on-device CPU path); both require a Gemini key.
    const bool preferCloud = m_appSettings && m_appSettings->cloudFallbackLlm();
    const bool useCloud    = (preferCloud || !m_localGpuAvailable) && !geminiKey.isEmpty();

    LlmEngine *engine;
    if (useCloud) {
        if (!m_localGpuAvailable)
            ppInfo() << "[LlmController] No GPU detected — AI Coach will use Gemini cloud;"
                     << "local model download skipped.";
        else
            ppInfo() << "[LlmController] Cloud fallback forced — AI Coach will use Gemini cloud.";
        m_usingCloudLlm = true;
        m_llmCloudKey   = geminiKey;
        engine = new GeminiLlmEngine(geminiKey);
    } else if (!m_localGpuAvailable) {
        ppWarn() << "[LlmController] No GPU and no Gemini API key — AI Coach unavailable.";
        engine = new LocalLlmEngine;  // will fail at load; coach stays in not-ready state
    } else {
        engine = new LocalLlmEngine;
    }

    m_worker = new LlmWorker(engine);
    m_worker->moveToThread(m_workerThread);
    connectWorkerSignals();
    m_workerThread->start();

    if (m_usingCloudLlm) {
        triggerModelLoad();
    } else if (modelFilesExist()) {
        triggerModelLoad();
    } else if (m_localGpuAvailable) {
        startDownload();
    }
}

LlmController::~LlmController()
{
    QMetaObject::invokeMethod(m_worker, [this]() {
        m_worker->stop();
        m_worker->moveToThread(QCoreApplication::instance()->thread());
    }, Qt::BlockingQueuedConnection);
    m_workerThread->quit();
    m_workerThread->wait();
    delete m_worker;
}

// ---------------------------------------------------------------------------
// Q_PROPERTY readers

QVariantList LlmController::conversation() const
{
    QVariantList out;
    out.reserve(m_history.size());
    for (const QVariantMap &msg : m_history)
        out.append(msg);
    return out;
}

// ---------------------------------------------------------------------------
// Q_INVOKABLEs

void LlmController::sendMessage(const QString &text)
{
    if (text.trimmed().isEmpty() || !m_llmReady || m_llmActive)
        return;

    // Append user turn.
    QVariantMap userMsg;
    userMsg[QStringLiteral("role")]    = QStringLiteral("user");
    userMsg[QStringLiteral("text")]    = text.trimmed();
    userMsg[QStringLiteral("partial")] = false;
    m_history.append(userMsg);

    // Append empty assistant placeholder (will be filled by token stream).
    QVariantMap assistantMsg;
    assistantMsg[QStringLiteral("role")]    = QStringLiteral("model");
    assistantMsg[QStringLiteral("text")]    = QString{};
    assistantMsg[QStringLiteral("partial")] = true;
    m_history.append(assistantMsg);

    emit conversationChanged();

    m_llmActive = true;
    emit llmActiveChanged();
    m_latencyTimer.restart();

    // Build history list excluding the empty assistant placeholder.
    QVariantList historyForEngine;
    for (int i = 0; i < m_history.size() - 1; ++i)
        historyForEngine.append(m_history.at(i));

    const QString systemPrompt = QString::fromLatin1(kCoachSystemPrompt);
    QMetaObject::invokeMethod(m_worker,
        [this, historyForEngine, systemPrompt]() {
            m_worker->chat(historyForEngine, systemPrompt);
        }, Qt::QueuedConnection);
}

void LlmController::stopGeneration()
{
    QMetaObject::invokeMethod(m_worker,
        [this]() { m_worker->stop(); }, Qt::QueuedConnection);
}

void LlmController::clearHistory()
{
    if (m_llmActive)
        stopGeneration();
    m_history.clear();
    emit conversationChanged();
}

void LlmController::toggleLlmBackend()
{
    // The persisted preference is the single source of truth — flipping it drives
    // applyCloudFallbackPref() (and keeps the Settings toggle in sync).
    if (!m_cloudToggleAvailable || !m_appSettings)
        return;
    // No local GPU → cloud is mandatory (toggle locked ON in the UI); guard here too
    // so the badge can't force an unusable local LLM.
    if (!m_localGpuAvailable)
        return;
    m_appSettings->setCloudFallbackLlm(!m_appSettings->cloudFallbackLlm());
}

void LlmController::applyCloudFallbackPref()
{
    if (!m_appSettings)
        return;
    const QString key = SecretsManager::read(QStringLiteral("geminiApiKey"));
    const bool want = (m_appSettings->cloudFallbackLlm() || !m_localGpuAvailable)
                      && !key.isEmpty();
    // Rebuild when the desired backend changes, OR when staying on cloud but the
    // key value changed (e.g. the user corrected a bad key).
    if (want == m_usingCloudLlm && (!want || key == m_llmCloudKey))
        return;
    if (want) {
        m_usingCloudLlm     = true;
        m_switchingToGemini = true;
        m_llmCloudKey       = key;
        switchToGemini(key);
    } else {
        m_llmCloudKey.clear();
        switchToLocal();
    }
}

void LlmController::refreshCloudAvailability()
{
    const bool avail = !SecretsManager::read(QStringLiteral("geminiApiKey")).isEmpty();
    if (avail != m_cloudToggleAvailable) {
        m_cloudToggleAvailable = avail;
        emit cloudLlmFallbackAvailableChanged();
    }
    // A newly-entered key may enable a force-cloud (or no-GPU mandatory) preference.
    applyCloudFallbackPref();
}

void LlmController::setVoiceInput(bool enabled)
{
    if (m_voiceInputEnabled == enabled)
        return;
    m_voiceInputEnabled = enabled;
    emit voiceInputEnabledChanged();
}

void LlmController::setVoiceOutput(bool enabled)
{
    if (m_voiceOutputEnabled == enabled)
        return;
    m_voiceOutputEnabled = enabled;
    emit voiceOutputEnabledChanged();
}

// ---------------------------------------------------------------------------
// Worker signal handlers

void LlmController::onBackendChanged(const QString &backend)
{
    m_llmBackend = backend;
    emit llmBackendChanged();

    // CPU-only local path → hot-swap to Gemini (same pattern as TtsController → Azure).
    if (!m_usingCloudLlm && !m_switchingToGemini && backend.isEmpty()) {
        const QString key = SecretsManager::read(QStringLiteral("geminiApiKey"));
        if (!key.isEmpty()) {
            m_usingCloudLlm     = true;
            m_switchingToGemini = true;
            QTimer::singleShot(0, this, [this, key]() { switchToGemini(key); });
        }
    }
}

void LlmController::onModelReady()
{
    if (m_switchingToGemini)
        return;  // suppressed — switchToGemini() will set llmReady after cloud is up

    m_llmReady = true;
    emit llmReadyChanged();
}

void LlmController::onModelFailed(const QString &error)
{
    ppWarn() << "[LlmController] model failed:" << error;
    m_switchingToGemini = false;
}

void LlmController::onTokenReady(const QString &token)
{
    if (m_history.isEmpty())
        return;

    QVariantMap &last = m_history.last();
    last[QStringLiteral("text")] =
        last.value(QStringLiteral("text")).toString() + token;
    emit conversationChanged();
}

void LlmController::onResponseReady(const QString &full)
{
    if (!m_history.isEmpty()) {
        QVariantMap &last = m_history.last();
        last[QStringLiteral("text")]    = full;
        last[QStringLiteral("partial")] = false;
        emit conversationChanged();
    }

    m_lastLlmLatencyMs = m_latencyTimer.elapsed();
    emit lastLlmLatencyMsChanged();

    m_llmActive = false;
    emit llmActiveChanged();

    emit responseReady(full);
}

void LlmController::onLlmError(const QString &message)
{
    ppWarn() << "[LlmController]" << message;
    if (m_llmActive) {
        m_llmActive = false;
        emit llmActiveChanged();
        if (!m_history.isEmpty() && m_history.last()
                .value(QStringLiteral("partial")).toBool()) {
            m_history.last()[QStringLiteral("partial")] = false;
            emit conversationChanged();
        }
    }
}

// ---------------------------------------------------------------------------
// Download handlers

void LlmController::onDownloadProgress(int fileIndex, int fileCount,
                                        qint64 bytesReceived, qint64 bytesTotal)
{
    // Overall progress: completed files + fraction of current file.
    const qreal fileFrac = (bytesTotal > 0)
        ? qreal(bytesReceived) / qreal(bytesTotal)
        : 0.0;
    m_downloadProgress = (fileIndex + fileFrac) / fileCount;
    emit downloadProgressChanged();

    setDownloadStatus(tr("Downloading model… file %1 of %2")
                      .arg(fileIndex + 1).arg(fileCount));
}

void LlmController::onDownloadFileComplete(const QString &path)
{
    ppInfo() << "[LlmController] downloaded" << path;
}

void LlmController::onDownloadFinished()
{
    m_downloading = false;
    emit downloadingChanged();
    m_downloadProgress = 1.0;
    emit downloadProgressChanged();
    setDownloadStatus(QString{});
    triggerModelLoad();
}

void LlmController::onDownloadFailed(const QString &error)
{
    ppWarn() << "[LlmController] download failed:" << error;
    m_downloading = false;
    emit downloadingChanged();
    setDownloadStatus(tr("Download failed: %1").arg(error));

    // Fall back to cloud if we have a key and can't get the local model.
    const QString key = SecretsManager::read(QStringLiteral("geminiApiKey"));
    if (!key.isEmpty()) {
        m_usingCloudLlm     = true;
        m_switchingToGemini = true;
        switchToGemini(key);
    }
}

// ---------------------------------------------------------------------------
// Private helpers

// ⚠ EVERY FILE, NOT A SENTINEL — AND THE SENTINEL WAS THE WORST POSSIBLE CHOICE.
//
// This asked only whether genai_config.json was present. That is 1.5 KB and the FIRST entry
// in kModelFiles, so it is the first thing to land and the last thing a failure can remove.
// An interrupted download — and 4.9 GB over a home connection gets interrupted — therefore
// leaves the check passing over a directory holding almost nothing.
//
// The failure that produces is permanent and silent. `false` here routes to startDownload();
// `true` routes to triggerModelLoad(), which fails inside ORT with
//     External data path does not exist: ".../model.onnx.data"
// and nothing ever retries, because the sentinel is still there next launch. Seen in the
// field: a directory stuck for three weeks holding genai_config.json and a 52 MB model.onnx
// out of a 4.9 GB set, re-reporting the same ORT error on every start.
//
// Checking presence is ENOUGH, and that is a property of the downloader rather than luck:
// ModelDownloader writes each item to "<file>.part" and renames to the final name only on
// success, removing the .part on failure (ModelDownloader.cpp:80,129-132,148-150). So a file
// under its real name is a COMPLETE file, and no size or checksum check is needed to
// distinguish a finished download from a torn one.
bool LlmController::modelFilesExist() const
{
    const QString dir = modelDataDir();
    for (const char *file : kModelFiles) {
        if (!QFile::exists(dir + QString::fromLatin1(file)))
            return false;
    }
    return true;
}

void LlmController::startDownload()
{
    const QString dir = modelDataDir();
    QDir().mkpath(dir);

    QList<ModelDownloader::Item> items;
    items.reserve(static_cast<int>(std::size(kModelFiles)));

    for (const char *file : kModelFiles) {
        ModelDownloader::Item item;
        item.url       = QUrl(QString::fromLatin1(kHfBase) + QString::fromLatin1(file));
        item.localPath = dir + QString::fromLatin1(file);
        items.append(item);
    }

    m_downloading = true;
    emit downloadingChanged();
    setDownloadStatus(tr("Starting download of Phi-4-mini (~4.9 GB)…"));
    m_downloader->download(items);
}

void LlmController::triggerModelLoad()
{
    const QString dir = modelDataDir();
    QMetaObject::invokeMethod(m_worker,
        [this, dir]() { m_worker->loadModel(dir); }, Qt::QueuedConnection);
}

void LlmController::setDownloadStatus(const QString &status)
{
    if (m_downloadStatus == status)
        return;
    m_downloadStatus = status;
    emit downloadStatusChanged();
}

void LlmController::connectWorkerSignals()
{
    connect(m_worker, &LlmWorker::backendChanged, this, &LlmController::onBackendChanged);
    connect(m_worker, &LlmWorker::modelReady,     this, &LlmController::onModelReady);
    connect(m_worker, &LlmWorker::modelFailed,    this, &LlmController::onModelFailed);
    connect(m_worker, &LlmWorker::tokenReady,     this, &LlmController::onTokenReady);
    connect(m_worker, &LlmWorker::responseReady,  this, &LlmController::onResponseReady);
    connect(m_worker, &LlmWorker::errorOccurred,  this, &LlmController::onLlmError);
}

void LlmController::switchToGemini(const QString &apiKey)
{
    m_downloader->abort();
    m_workerThread->quit();
    m_workerThread->wait();
    delete m_worker;
    m_worker = nullptr;

    auto *engine = new GeminiLlmEngine(apiKey);
    m_worker     = new LlmWorker(engine);
    m_worker->moveToThread(m_workerThread);
    connectWorkerSignals();
    m_workerThread->start();
    triggerModelLoad();
}

void LlmController::switchToLocal()
{
    m_workerThread->quit();
    m_workerThread->wait();
    delete m_worker;
    m_worker = nullptr;

    m_usingCloudLlm     = false;
    m_switchingToGemini = false;
    m_llmReady          = false;
    emit llmReadyChanged();

    auto *engine = new LocalLlmEngine;
    m_worker     = new LlmWorker(engine);
    m_worker->moveToThread(m_workerThread);
    connectWorkerSignals();
    m_workerThread->start();

    if (modelFilesExist())
        triggerModelLoad();
    else if (LocalLlmEngine::hasGpu())
        startDownload();
    else
        ppWarn() << "[LlmController] switchToLocal: no GPU — download skipped.";
}

QString LlmController::modelDataDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/models/llm/phi4-mini/");
}
