#pragma once

#include <QObject>
#include <QString>
#include <QThread>

class AudioInput;
class AudioInputBase;
class AudioStreamSaver;
class WhisperProcessor;

// Owns the audio-capture and STT-processor threads and exposes the growing
// transcript as a Q_PROPERTY so QML can bind to it directly.
class TranscriptionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString transcript  READ transcript  NOTIFY transcriptChanged)
    Q_PROPERTY(bool   isListening READ isListening NOTIFY isListeningChanged)

public:
    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    QString transcript()  const { return m_transcript; }
    bool    isListening() const { return m_listening; }

public slots:
    void startListening();
    void stopListening();

signals:
    void transcriptChanged();
    void isListeningChanged();

private slots:
    void onTranscriptionReceived(const QString &text);
    void onAudioError(const QString &message);
    void onSTTError(const QString &message);
    void startAudio();   // called once microphone permission is confirmed

private:
    QThread          *m_audioThread;
    QThread          *m_processorThread;
    AudioInput       *m_audioInput;
    AudioStreamSaver *m_streamSaver;
    WhisperProcessor *m_whisper;
    QString           m_transcript;
    bool              m_listening = false;
};
