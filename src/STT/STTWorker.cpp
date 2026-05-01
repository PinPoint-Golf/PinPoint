#include "STTWorker.h"
#include "STTBackend.h"
#include <QDebug>

STTWorker::STTWorker(STTBackend* backend, QObject* parent)
  : QObject(parent), m_backend(backend)
{
    // Backend becomes a child so it moves to the worker thread automatically
    // when this object is moveToThread()'d.
    m_backend->setParent(this);

    connect(m_backend, &STTBackend::transcriptionReady,
            this,      &STTWorker::transcriptionReady);
    connect(m_backend, &STTBackend::transcriptionFailed,
            this,      &STTWorker::transcriptionFailed);
}

void STTWorker::loadModel(const QString& modelPath)
{
    if (m_backend->loadModel(modelPath))
        emit modelReady();
    else
        emit modelFailed(QStringLiteral("Failed to load model from: ") + modelPath);
}

void STTWorker::transcribe(const std::vector<float>& pcmF32)
{
    m_backend->transcribe(pcmF32);
}
