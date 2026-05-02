#pragma once
#include "TTSEngine.h"
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// TTSEngine backend that synthesises speech via Azure Cognitive Services TTS.
// Used automatically on CPU-only builds when an Azure TTS API key is available.
// Stores key under SecretsManager "azureTtsApiKey" / env AZURE_TTS_API_KEY.
//
// Connection lifecycle (one HTTP POST per synthesise() call):
//   - No persistent connection — each call opens, streams, and closes.
//   - stop() aborts any in-flight request; audioReady() will not fire for it.
class AzureTTSEngine : public TTSEngine {
    Q_OBJECT
public:
    explicit AzureTTSEngine(const QString &apiKey, QObject *parent = nullptr);

    bool    loadModel(const QString &, const QString &, const QString &) override;
    void    synthesise(const QString &text) override;
    void    stop() override;
    bool    isReady()     const override { return m_ready; }
    QString gpuBackend()  const override { return QStringLiteral("Cloud"); }

private:
    QString buildSsml(const QString &text) const;

    QString                m_apiKey;
    QNetworkAccessManager *m_nam   = nullptr;
    QNetworkReply         *m_reply = nullptr;
    bool                   m_ready = false;
};
