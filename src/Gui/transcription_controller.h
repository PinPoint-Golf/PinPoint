#pragma once

#include <QObject>
#include <QString>
#include <QThread>

class AudioInput;
class AudioInputBase;
class AudioStreamSaver;
class STTProcessor;

// Owns the audio-capture and STT-processor threads and exposes the growing
// transcript as a Q_PROPERTY so QML can bind to it directly.
class TranscriptionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString transcript              READ transcript              NOTIFY transcriptChanged)
    Q_PROPERTY(bool   isListening             READ isListening             NOTIFY isListeningChanged)
    Q_PROPERTY(QString sttBackend             READ sttBackend              NOTIFY sttBackendChanged)
    Q_PROPERTY(bool   cloudSttFallbackAvailable READ cloudSttFallbackAvailable NOTIFY cloudSttFallbackAvailableChanged)

public:
    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    QString transcript()              const { return m_transcript; }
    bool    isListening()             const { return m_listening; }
    QString sttBackend()              const { return m_sttBackend; }
    bool    cloudSttFallbackAvailable() const { return m_sttCloudToggleAvailable; }

public slots:
    void startListening();
    void stopListening();

    Q_INVOKABLE void toggleSttBackend();

signals:
    void transcriptChanged();
    void isListeningChanged();
    void sttBackendChanged();
    void cloudSttFallbackAvailableChanged();

private slots:
    void onTranscriptionReceived(const QString &text);
    void onBackendLabelReady(const QString &label);
    void onAudioError(const QString &message);
    void onSTTError(const QString &message);
    void startAudio();   // called once microphone permission is confirmed

private:
    QThread          *m_audioThread;
    QThread          *m_processorThread;
    AudioInput       *m_audioInput;
    AudioStreamSaver *m_streamSaver;
    STTProcessor     *m_stt;
    QString           m_transcript;
    QString           m_sttBackend;
    bool              m_listening               = false;
    bool              m_sttUsingCloud           = false;
    bool              m_sttCloudToggleAvailable = false;
};
