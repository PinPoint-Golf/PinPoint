#include "AzureTTSEngine.h"
#include <QAudioFormat>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <cstdint>

// Azure Speech Service TTS endpoint (UK West region).
// Format: https://{region}.tts.speech.microsoft.com/cognitiveservices/v1
static const char kAzureEndpoint[] =
    "https://ukwest.tts.speech.microsoft.com/cognitiveservices/v1";

// Azure TTS voice used for synthesis.
static const char kAzureVoice[] = "en-GB-SoniaNeural";

static QString xmlEscape(const QString &text)
{
    QString r = text;
    r.replace(QLatin1Char('&'),  QLatin1String("&amp;"));
    r.replace(QLatin1Char('<'),  QLatin1String("&lt;"));
    r.replace(QLatin1Char('>'),  QLatin1String("&gt;"));
    r.replace(QLatin1Char('"'),  QLatin1String("&quot;"));
    r.replace(QLatin1Char('\''), QLatin1String("&apos;"));
    return r;
}

AzureTTSEngine::AzureTTSEngine(const QString &apiKey, QObject *parent)
    : TTSEngine(parent)
    , m_apiKey(apiKey)
{}

bool AzureTTSEngine::loadModel(const QString &, const QString &, const QString &)
{
    m_nam   = new QNetworkAccessManager(this);
    m_ready = true;
    emit modelLoaded();
    return true;
}

void AzureTTSEngine::synthesise(const QString &text)
{
    if (!m_ready || text.trimmed().isEmpty()) return;

    emit synthesisStarted();

    QNetworkRequest req{QUrl(QString::fromLatin1(kAzureEndpoint))};
    req.setRawHeader("Ocp-Apim-Subscription-Key", m_apiKey.toLatin1());
    req.setRawHeader("Content-Type",               "application/ssml+xml");
    req.setRawHeader("X-Microsoft-OutputFormat",   "raw-24khz-16bit-mono-pcm");

    m_reply = m_nam->post(req, buildSsml(text).toUtf8());
    QNetworkReply *reply = m_reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_reply == reply)
            m_reply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError)
                emit errorOccurred(reply->errorString());
            emit synthesisFinished();
            reply->deleteLater();
            return;
        }

        const QByteArray pcm16 = reply->readAll();
        reply->deleteLater();

        if (!pcm16.isEmpty()) {
            const int sampleCount = pcm16.size() / 2;
            QByteArray pcmFloat(sampleCount * static_cast<int>(sizeof(float)),
                                Qt::Uninitialized);
            const auto *src = reinterpret_cast<const int16_t *>(pcm16.constData());
            auto       *dst = reinterpret_cast<float *>(pcmFloat.data());
            for (int i = 0; i < sampleCount; ++i)
                dst[i] = src[i] / 32768.0f;

            QAudioFormat fmt;
            fmt.setSampleRate(24000);
            fmt.setChannelCount(1);
            fmt.setSampleFormat(QAudioFormat::Float);
            emit audioReady(pcmFloat, fmt);
        }
        emit synthesisFinished();
    });
}

void AzureTTSEngine::stop()
{
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
    }
}

QString AzureTTSEngine::buildSsml(const QString &text) const
{
    const int pct = static_cast<int>((m_speed - 1.0f) * 100.0f);
    const QString rate = (pct >= 0)
        ? QStringLiteral("+%1%").arg(pct)
        : QStringLiteral("%1%").arg(pct);

    return QStringLiteral(
        "<speak version='1.0' xml:lang='en-GB'"
        " xmlns='http://www.w3.org/2001/10/synthesis'>"
        "<voice name='%1'>"
        "<prosody rate='%2'>%3</prosody>"
        "</voice></speak>")
        .arg(QString::fromLatin1(kAzureVoice), rate, xmlEscape(text));
}
