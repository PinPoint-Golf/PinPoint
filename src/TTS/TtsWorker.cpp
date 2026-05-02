#include "TtsWorker.h"
#include "TTSEngine.h"

TtsWorker::TtsWorker(TTSEngine *engine, QObject *parent)
    : QObject(parent), m_engine(engine)
{
    // Reparenting causes the engine to follow this object onto its QThread.
    m_engine->setParent(this);

    connect(m_engine, &TTSEngine::audioReady,        this, &TtsWorker::audioReady);
    connect(m_engine, &TTSEngine::synthesisStarted,  this, &TtsWorker::synthesisStarted);
    connect(m_engine, &TTSEngine::synthesisFinished, this, &TtsWorker::synthesisFinished);
    connect(m_engine, &TTSEngine::errorOccurred,     this, &TtsWorker::errorOccurred);
    // modelLoaded is NOT forwarded here — loadModel() emits backendChanged then
    // modelReady explicitly so the controller always sees backend before ready.
}

void TtsWorker::loadModel(const QString &modelPath,
                           const QString &voicePath,
                           const QString &tokensPath)
{
    if (!m_engine->loadModel(modelPath, voicePath, tokensPath)) {
        emit modelFailed(QStringLiteral("loadModel failed — see errorOccurred for details"));
        return;
    }
    // Emit backendChanged first so the controller can decide (e.g. switch to
    // cloud TTS) before modelReady marks the engine as usable.
    emit backendChanged(m_engine->gpuBackend());
    emit modelReady();
}

void TtsWorker::synthesise(const QString &text)
{
    m_engine->synthesise(text);
}

void TtsWorker::stop()
{
    m_engine->stop();
}
