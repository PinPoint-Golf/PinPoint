#include "transcription_controller.h"

#include "audio_input.h"
#include "audio_input_base.h"
#include "audio_stream_saver.h"
#include "whisper_processor.h"

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
    , m_streamSaver(new AudioStreamSaver(this))
    , m_whisper(new WhisperProcessor)
{
    m_audioThread->setObjectName(QStringLiteral("AudioThread"));
    m_processorThread->setObjectName(QStringLiteral("ProcessorThread"));

    m_audioInput->moveToThread(m_audioThread);
    m_whisper->moveToThread(m_processorThread);

    m_audioInput->connectProcessor(m_whisper);

    // audioDataReady is emitted on m_audioThread; m_streamSaver lives on the
    // main thread, so this becomes a queued connection automatically.
    connect(m_audioInput, &AudioInputBase::audioDataReady,
            m_streamSaver, &AudioStreamSaver::onAudioData);

    connect(m_whisper, &WhisperProcessor::transcriptionReceived,
            this, &TranscriptionController::onTranscriptionReceived);
    connect(m_whisper, &WhisperProcessor::errorOccurred,
            this, &TranscriptionController::onSTTError);
    connect(m_audioInput, &AudioInputBase::errorOccurred,
            this, &TranscriptionController::onAudioError);

    connect(m_processorThread, &QThread::started,
            m_whisper, &WhisperProcessor::start);
    connect(m_audioThread, &QThread::started,
            m_audioInput, [this]() { m_audioInput->start(); });

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
        QMetaObject::invokeMethod(m_audioInput, [this]() {
            m_audioInput->stop();
        }, Qt::BlockingQueuedConnection);

        m_audioThread->quit();
        m_audioThread->wait();
    }

    // Move back to the main thread before deleting so Qt's internal timer
    // cleanup runs on the correct thread (avoids "cannot be stopped from
    // another thread" warnings from QAudioSource's FFmpeg backend).
    m_audioInput->moveToThread(QThread::currentThread());
    delete m_audioInput;
    m_audioInput = nullptr;

    // Finalise the WAV file now that the audio thread has stopped producing data.
    m_streamSaver->stopSaving();

    m_processorThread->quit();
    m_processorThread->wait();
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

void TranscriptionController::startAudio()
{
    m_audioThread->start();
}
