#include "transcription_controller.h"

#include "audio_input.h"
#include "audio_input_base.h"
#include "audio_stream_saver.h"
#include "whisper_processor.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QThread>

#ifdef Q_OS_MACOS
#include "macos_permissions.h"
#endif

TranscriptionController::TranscriptionController(QObject *parent)
    : QObject(parent)
    , m_audioThread(new QThread(this))
    , m_processorThread(new QThread(this))
    , m_audioInput(new AudioInput)
    , m_streamSaver(nullptr)
    , m_whisper(new WhisperProcessor)
{
    m_audioThread->setObjectName(QStringLiteral("AudioThread"));
    m_processorThread->setObjectName(QStringLiteral("ProcessorThread"));

    m_audioInput->moveToThread(m_audioThread);
    m_whisper->moveToThread(m_processorThread);

    m_audioInput->connectProcessor(m_whisper);

    // connect(m_audioInput, &AudioInputBase::audioDataReady,
    //         m_streamSaver, &AudioStreamSaver::onAudioData);

    connect(m_whisper, &WhisperProcessor::transcriptionReceived,
            this, &TranscriptionController::onTranscriptionReceived);
    connect(m_whisper, &WhisperProcessor::errorOccurred,
            this, &TranscriptionController::onSTTError);
    connect(m_audioInput, &AudioInputBase::errorOccurred,
            this, &TranscriptionController::onAudioError);

    connect(m_processorThread, &QThread::started,
            m_whisper, &WhisperProcessor::start);

    m_processorThread->start();

#ifdef Q_OS_MACOS
    auto *self = this;
    requestMicrophonePermission([self](bool granted) {
        QMetaObject::invokeMethod(self, [self, granted]() {
            if (granted)
                self->startAudio();
            else
                qWarning() << "[TranscriptionController] Microphone permission denied";
        }, Qt::QueuedConnection);
    });
#else
    startAudio();
#endif
}

TranscriptionController::~TranscriptionController()
{
    if (m_audioThread->isRunning()) {
        // Stop capture and move the object back to the main thread while the
        // audio thread is still running — moveToThread must be called from the
        // object's current thread, so we do both steps in the same invoke.
        QMetaObject::invokeMethod(m_audioInput, [this]() {
            m_audioInput->stop();
            m_audioInput->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);

        m_audioThread->quit();
        m_audioThread->wait();
    }

    delete m_audioInput;
    m_audioInput = nullptr;

    // m_streamSaver->stopSaving();

    if (m_processorThread->isRunning()) {
        // Move m_whisper (and its child m_flushTimer) back to the main thread
        // while the processor thread is still running, so the QTimer destructor
        // runs on the correct thread and does not trigger killTimer warnings.
        QMetaObject::invokeMethod(m_whisper, [this]() {
            m_whisper->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);

        m_processorThread->quit();
        m_processorThread->wait();
    }
    delete m_whisper;
    m_whisper = nullptr;
}

void TranscriptionController::onTranscriptionReceived(const QString &text)
{
    if (!m_transcript.isEmpty())
        m_transcript += QLatin1Char('\n');
    m_transcript += text;
    emit transcriptChanged();
}

void TranscriptionController::onAudioError(const QString &message)
{
    qWarning() << "[Audio]" << message;
}

void TranscriptionController::onSTTError(const QString &message)
{
    qWarning() << "[STT]" << message;
}

void TranscriptionController::startListening()
{
    if (m_listening || !m_audioThread->isRunning())
        return;
    QMetaObject::invokeMethod(m_audioInput, [this]() {
        m_audioInput->start();
    }, Qt::QueuedConnection);
    m_listening = true;
    emit isListeningChanged();
}

void TranscriptionController::stopListening()
{
    if (!m_listening)
        return;
    QMetaObject::invokeMethod(m_audioInput, [this]() {
        m_audioInput->stop();
    }, Qt::QueuedConnection);
    m_listening = false;
    emit isListeningChanged();
}

void TranscriptionController::startAudio()
{
    m_audioThread->start();
}
