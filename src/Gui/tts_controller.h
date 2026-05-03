#pragma once
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QThread>

class AudioOutput;
class ModelDownloader;
class TtsWorker;

// Owns the TTS synthesis thread, audio output pipeline, and model downloader.
// Exposes the full TTS workflow to QML via Q_PROPERTYs and Q_INVOKABLEs.
//
// On construction the controller checks whether the Kokoro model files are
// present under modelDataDir().  If not, it downloads them automatically
// before loading the model.
//
// QML usage:
//   ttsController.speak("Hello world")
//   ttsController.stopSpeaking()
//   ttsController.ttsReady         // bool — model loaded, speak() may be called
//   ttsController.ttsActive        // bool — synthesis in progress
//   ttsController.downloading      // bool — model files being fetched
//   ttsController.downloadProgress // real 0.0–1.0
//   ttsController.downloadStatus   // human-readable status string
//   ttsController.voices           // string list of available voices
//   ttsController.voice            // current voice name (read/write)

class TtsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool        ttsReady                  READ ttsReady                  NOTIFY ttsReadyChanged)
    Q_PROPERTY(bool        ttsActive                 READ ttsActive                 NOTIFY ttsActiveChanged)
    Q_PROPERTY(bool        canReplay                 READ canReplay                 NOTIFY canReplayChanged)
    Q_PROPERTY(bool        downloading               READ downloading               NOTIFY downloadingChanged)
    Q_PROPERTY(qreal       downloadProgress          READ downloadProgress          NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString     downloadStatus            READ downloadStatus            NOTIFY downloadStatusChanged)
    Q_PROPERTY(QString     ttsBackend                READ ttsBackend                NOTIFY ttsBackendChanged)
    Q_PROPERTY(bool        cloudTtsFallbackAvailable READ cloudTtsFallbackAvailable NOTIFY cloudTtsFallbackAvailableChanged)
    Q_PROPERTY(QStringList voices                    READ voices                    CONSTANT)
    Q_PROPERTY(QString     voice                     READ voice                     WRITE setVoice NOTIFY voiceChanged)
    Q_PROPERTY(qint64      lastTtsLatencyMs          READ lastTtsLatencyMs          NOTIFY lastTtsLatencyMsChanged)

public:
    explicit TtsController(QObject *parent = nullptr);
    ~TtsController() override;

    bool        ttsReady()                  const { return m_ttsReady; }
    bool        ttsActive()                 const { return m_ttsActive; }
    bool        canReplay()                 const { return !m_lastAudioCache.isEmpty(); }
    bool        downloading()               const { return m_downloading; }
    qreal       downloadProgress()          const { return m_downloadProgress; }
    QString     downloadStatus()            const { return m_downloadStatus; }
    QString     ttsBackend()               const { return m_ttsBackend; }
    bool        cloudTtsFallbackAvailable() const { return m_cloudToggleAvailable; }
    QStringList voices()                    const;
    QString     voice()                     const { return m_voice; }
    void        setVoice(const QString &voice);
    qint64      lastTtsLatencyMs()          const { return m_lastTtsLatencyMs; }

    Q_INVOKABLE void speak(const QString &text);
    Q_INVOKABLE void stopSpeaking();
    Q_INVOKABLE void replayLastAudio();
    Q_INVOKABLE void toggleTtsBackend();

signals:
    void ttsReadyChanged();
    void ttsActiveChanged();
    void canReplayChanged();
    void downloadingChanged();
    void downloadProgressChanged();
    void downloadStatusChanged();
    void ttsBackendChanged();
    void cloudTtsFallbackAvailableChanged();
    void voiceChanged();
    void lastTtsLatencyMsChanged();

private slots:
    void onModelReady();
    void onModelFailed(const QString &error);
    void onBackendChanged(const QString &backend);
    void onAudioReady(const QByteArray &data, const QAudioFormat &format);
    void onSynthesisStarted();
    void onSynthesisFinished();
    void onTtsError(const QString &message);

    void onDownloadProgress(int fileIndex, int fileCount,
                            qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFileComplete(const QString &path);
    void onDownloadFinished();
    void onDownloadFailed(const QString &error);

private:
    bool    modelFilesExist() const;
    void    startDownload();
    void    triggerModelLoad();
    void    setDownloadStatus(const QString &status);
    void    connectWorkerSignals();
    void    switchToAzure(const QString &apiKey);
    void    switchToKokoro();

    QString modelDataDir()    const;
    QString resolveModelPath()  const;
    QString resolveVoicePath()  const;
    QString resolveTokensPath() const;

    QThread         *m_workerThread;
    TtsWorker       *m_worker;
    AudioOutput     *m_audioOutput;
    ModelDownloader *m_downloader;

    bool          m_ttsReady             = false;
    bool          m_ttsActive            = false;
    bool          m_downloading          = false;
    bool          m_usingCloudTts        = false;
    bool          m_switchingToAzure     = false;
    bool          m_forceLocalTts        = false;
    bool          m_cloudToggleAvailable = false;
    qreal         m_downloadProgress = 0.0;
    QString       m_downloadStatus;
    QString       m_ttsBackend;
    QString       m_voice = QStringLiteral("af_sky");
    QByteArray    m_lastAudioCache;
    QAudioFormat  m_lastAudioFormat;
    QElapsedTimer m_ttsStartTimer;
    qint64        m_lastTtsLatencyMs = 0;
};
