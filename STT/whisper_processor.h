#pragma once

#include "audio_processor_base.h"
#include <QAudioFormat>
#include <QByteArray>
#include <QNetworkAccessManager>

class QTimer;
class QNetworkReply;

// Forwards buffered PCM audio to a whisper.cpp inference server running on
// localhost:5001.  Audio is accumulated for chunkDurationMs() milliseconds,
// wrapped in a WAV envelope, then POST-ed to /inference as multipart/form-data.
// Overlapping requests are suppressed: if a request is still in flight when the
// flush timer fires the current buffer is held and sent on the next cycle.
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

    // Audio is sent to the server in chunks of this duration (default: 3000 ms).
    void setChunkDurationMs(int ms);
    int  chunkDurationMs() const { return m_chunkDurationMs; }

    // Chunks whose RMS amplitude (normalised 0.0–1.0) falls below this value
    // are discarded without being sent to the server (default: 0.01 ≈ -40 dBFS).
    void   setSilenceThreshold(double threshold) { m_silenceThreshold = threshold; }
    double silenceThreshold() const { return m_silenceThreshold; }

public slots:
    // Call after moveToThread() to start the flush timer in the correct thread.
    void start();
    void processAudio(const QByteArray &data, const QAudioFormat &format) override;

signals:
    void transcriptionReceived(const QString &text);
    void errorOccurred(const QString &message);

private slots:
    void onFlushTimer();
    void onReplyFinished(QNetworkReply *reply);

private:
    QByteArray buildWav(const QByteArray &pcm, const QAudioFormat &fmt) const;
    void       sendChunk(const QByteArray &pcm);
    double     computeRms(const QByteArray &pcm, const QAudioFormat &fmt) const;

    QNetworkAccessManager *m_network;
    QTimer                *m_flushTimer;
    QByteArray             m_buffer;
    QAudioFormat           m_format;
    int                    m_chunkDurationMs  = 3000;
    bool                   m_requestInFlight  = false;
    double                 m_silenceThreshold = 0.01;
};
