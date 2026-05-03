#include "STTBackendAzure.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <cstdint>

// Azure Cognitive Services Speech-to-Text REST endpoint (UK West region).
// Format: https://{region}.stt.speech.microsoft.com/speech/recognition/{mode}/cognitiveservices/v1
static const char kAzureEndpoint[] =
    "https://ukwest.stt.speech.microsoft.com/speech/recognition/conversation"
    "/cognitiveservices/v1?language=en-GB&format=simple";

STTBackendAzure::STTBackendAzure(const QString& apiKey, QObject* parent)
    : STTBackend(parent)
    , m_apiKey(apiKey)
{}

bool STTBackendAzure::loadModel(const QString&)
{
    // QNetworkAccessManager must be created on the thread it will be used from.
    // loadModel() is invoked from the worker thread, so affinity is correct here.
    m_nam = new QNetworkAccessManager(this);
    return true;
}

bool STTBackendAzure::isReady() const
{
    return m_nam != nullptr;
}

void STTBackendAzure::transcribe(const std::vector<float>& pcmF32)
{
    if (!m_nam || pcmF32.empty()) return;
    if (m_reply) return;  // drop chunk if a request is still in flight

    QNetworkRequest req{QUrl(QString::fromLatin1(kAzureEndpoint))};
    req.setRawHeader("Ocp-Apim-Subscription-Key", m_apiKey.toLatin1());
    req.setRawHeader("Content-Type", "audio/wav; codec=audio/pcm; samplerate=16000");
    req.setRawHeader("Accept",       "application/json");

    m_reply = m_nam->post(req, buildWav(pcmF32));
    QNetworkReply* reply = m_reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_reply == reply)
            m_reply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError)
                emit transcriptionFailed(reply->errorString());
            reply->deleteLater();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();

        if (!doc.isObject()) return;
        const QJsonObject obj = doc.object();

        if (obj[QLatin1String("RecognitionStatus")].toString() != QLatin1String("Success"))
            return;

        const QString text = obj[QLatin1String("DisplayText")].toString().trimmed();
        if (!text.isEmpty())
            emit transcriptionReady(text);
    });
}

void STTBackendAzure::stopStreaming()
{
    if (m_reply) {
        QNetworkReply* r = m_reply;
        m_reply = nullptr;
        r->abort();
    }
}

// Wraps raw float32 PCM (16 kHz mono) in a RIFF WAV container with a 16-bit PCM body.
QByteArray STTBackendAzure::buildWav(const std::vector<float>& pcmF32)
{
    const quint32 dataSize = static_cast<quint32>(pcmF32.size() * sizeof(int16_t));
    const quint32 riffSize = 36 + dataSize;

    QByteArray wav;
    wav.reserve(static_cast<qsizetype>(44 + dataSize));

    auto w32 = [&](quint32 v) {
        wav.append(static_cast<char>(v         & 0xFF));
        wav.append(static_cast<char>((v >>  8) & 0xFF));
        wav.append(static_cast<char>((v >> 16) & 0xFF));
        wav.append(static_cast<char>((v >> 24) & 0xFF));
    };
    auto w16 = [&](quint16 v) {
        wav.append(static_cast<char>(v        & 0xFF));
        wav.append(static_cast<char>((v >> 8) & 0xFF));
    };

    wav.append("RIFF", 4); w32(riffSize);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4); w32(16);
    w16(1);      // PCM
    w16(1);      // mono
    w32(16000);  // sample rate
    w32(32000);  // byte rate  = 16000 * 1 * 2
    w16(2);      // block align = 1 * 2
    w16(16);     // bits per sample
    wav.append("data", 4); w32(dataSize);

    const qsizetype headerEnd = wav.size();  // 44
    wav.resize(static_cast<qsizetype>(headerEnd + dataSize));
    auto* dst = reinterpret_cast<int16_t*>(wav.data() + headerEnd);
    for (size_t i = 0; i < pcmF32.size(); ++i) {
        const float s = std::max(-1.0f, std::min(1.0f, pcmF32[i]));
        dst[i] = static_cast<int16_t>(s * 32767.0f);
    }
    return wav;
}
