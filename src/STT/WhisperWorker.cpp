#include "WhisperWorker.h"
#include "WhisperBackend.h"
#include <QDebug>

WhisperWorker::WhisperWorker(WhisperBackend* backend, QObject* parent)
  : QObject(parent), m_backend(backend)
{
    // Backend becomes a child so it moves to the worker thread automatically
    // when this object is moveToThread()'d.
    m_backend->setParent(this);

    connect(m_backend, &WhisperBackend::transcriptionReady,
            this,      &WhisperWorker::transcriptionReady);
    connect(m_backend, &WhisperBackend::transcriptionFailed,
            this,      &WhisperWorker::transcriptionFailed);
}

void WhisperWorker::loadModel(const QString& modelPath)
{
    if (m_backend->loadModel(modelPath))
        emit modelReady();
    else
        emit modelFailed(QStringLiteral("Failed to load model from: ") + modelPath);
}

void WhisperWorker::transcribe(const std::vector<float>& pcmF32)
{
    // whisper_full() blocks here for the duration of inference.
    // This slot runs on the worker thread, so the processor and audio
    // threads are never blocked.
    m_backend->transcribe(pcmF32);
}
