#pragma once

#include "audio_processor_base.h"
#include <QAudioFormat>
#include <QByteArray>

class QThread;
class QTimer;
class WhisperWorker;

// Buffers PCM audio from the capture pipeline and dispatches it to an in-process
// whisper.cpp worker thread for transcription.  Audio is accumulated for
// chunkDurationMs() milliseconds, silence-gated, converted to 16 kHz mono float32
// via AudioConverter, then handed to WhisperWorker::transcribe() via a queued
// invocation so the CPU-bound whisper_full() call never blocks this thread.
//
// Typical wiring:
//   auto *whisper = new WhisperProcessor(this);
//   input->connectProcessor(whisper);
//   connect(whisper, &WhisperProcessor::transcriptionReceived,
//           this, &MyClass::onText);
//   input->start();

class WhisperProcessor : public AudioProcessorBase
{
    Q_OBJECT

public:
    explicit WhisperProcessor(QObject *parent = nullptr);
    ~WhisperProcessor() override;

    // Audio is dispatched to the worker in chunks of this duration (default: 3000 ms).
    void setChunkDurationMs(int ms);
    int  chunkDurationMs() const { return m_chunkDurationMs; }

    // Chunks whose RMS amplitude (normalised 0.0–1.0) falls below this value
    // are discarded without being dispatched (default: 0.01 ≈ -40 dBFS).
    void   setSilenceThreshold(double threshold) { m_silenceThreshold = threshold; }
    double silenceThreshold() const { return m_silenceThreshold; }

public slots:
    // Call after moveToThread() to start the flush timer in the correct thread.
    void start();
    void processAudio(const QByteArray &data, const QAudioFormat &format) override;

signals:
    void transcriptionReceived(const QString &text);
    void errorOccurred(const QString &message);
    void modelReady();
    void modelNotFound(const QStringList &searchedPaths);

private slots:
    void onFlushTimer();

private:
    // Resolution helpers — see resolveModelPath() in .cpp for the search order.
    QStringList modelCandidates(const QString &filename) const;
    QString     resolveModelPath(const QString &filename) const;
    double      computeRms(const QByteArray &pcm, const QAudioFormat &fmt) const;

    QTimer        *m_flushTimer;
    QThread       *m_workerThread     = nullptr;
    WhisperWorker *m_worker           = nullptr;
    QByteArray     m_buffer;
    QAudioFormat   m_format;
    QString        m_modelPath;        // resolved in ctor, used in start()
    QStringList    m_searchedPaths;    // populated when model is not found
    int            m_chunkDurationMs  = 3000;
    double         m_silenceThreshold = 0.01;
};
