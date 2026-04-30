#pragma once
#include <QAudioFormat>
#include <QObject>
#include <QString>

class TTSEngine;

// Thin QObject wrapper that binds a TTSEngine to a dedicated QThread.
// All public slots are safe to invoke via queued signal-slot connections
// or QMetaObject::invokeMethod from any other thread.
//
// The engine is reparented to this object so it moves with it when
// moveToThread() is called.

class TtsWorker : public QObject
{
    Q_OBJECT

public:
    explicit TtsWorker(TTSEngine *engine, QObject *parent = nullptr);

public slots:
    void loadModel(const QString &modelPath,
                   const QString &voicePath,
                   const QString &tokensPath);
    void synthesise(const QString &text);
    void stop();

signals:
    void modelReady();
    void modelFailed(const QString &error);
    void synthesisStarted();
    void synthesisFinished();
    void audioReady(const QByteArray &pcmData, const QAudioFormat &format);
    void errorOccurred(const QString &error);

private:
    TTSEngine *m_engine;
};
