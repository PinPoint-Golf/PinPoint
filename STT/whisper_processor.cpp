#include "whisper_processor.h"

#include <QDataStream>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

WhisperProcessor::WhisperProcessor(QObject *parent)
    : AudioProcessorBase(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_flushTimer(new QTimer(this))
{
    m_flushTimer->setInterval(m_chunkDurationMs);
    connect(m_flushTimer, &QTimer::timeout, this, &WhisperProcessor::onFlushTimer);
    m_flushTimer->start();
}

WhisperProcessor::~WhisperProcessor()
{
    m_flushTimer->stop();
}

void WhisperProcessor::setChunkDurationMs(int ms)
{
    m_chunkDurationMs = ms;
    m_flushTimer->setInterval(ms);
}

void WhisperProcessor::processAudio(const QByteArray &data, const QAudioFormat &format)
{
    m_format = format;
    m_buffer.append(data);
}

void WhisperProcessor::onFlushTimer()
{
    if (m_buffer.isEmpty() || m_requestInFlight)
        return;

    QByteArray chunk;
    chunk.swap(m_buffer);
    sendChunk(chunk);
}

void WhisperProcessor::sendChunk(const QByteArray &pcm)
{
    const QByteArray wav = buildWav(pcm, m_format);

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                       QVariant(QStringLiteral("audio/wav")));
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"audio.wav\"")));
    filePart.setBody(wav);
    multiPart->append(filePart);

    QNetworkRequest request{QUrl(QStringLiteral("http://localhost:5001/inference"))};
    QNetworkReply *reply = m_network->post(request, multiPart);
    multiPart->setParent(reply);

    m_requestInFlight = true;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onReplyFinished(reply);
    });
}

void WhisperProcessor::onReplyFinished(QNetworkReply *reply)
{
    m_requestInFlight = false;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit errorOccurred(reply->errorString());
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        emit errorOccurred(QStringLiteral("Unexpected response from whisper server"));
        return;
    }

    const QString text = doc.object().value(QStringLiteral("text")).toString().trimmed();
    if (!text.isEmpty())
        emit transcriptionReceived(text);
}

// Wraps raw PCM bytes in a minimal RIFF/WAV envelope so whisper can decode it.
QByteArray WhisperProcessor::buildWav(const QByteArray &pcm, const QAudioFormat &fmt) const
{
    const quint16 channels      = static_cast<quint16>(fmt.channelCount());
    const quint32 sampleRate    = static_cast<quint32>(fmt.sampleRate());
    const quint16 bitsPerSample = static_cast<quint16>(fmt.bytesPerSample() * 8);
    const quint32 byteRate      = sampleRate * channels * (bitsPerSample / 8);
    const quint16 blockAlign    = static_cast<quint16>(channels * (bitsPerSample / 8));
    const quint32 dataSize      = static_cast<quint32>(pcm.size());

    QByteArray wav;
    wav.reserve(44 + static_cast<int>(dataSize));
    QDataStream ds(&wav, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    ds.writeRawData("RIFF", 4);
    ds << quint32(36 + dataSize);   // total file size minus the 8-byte RIFF preamble
    ds.writeRawData("WAVE", 4);

    ds.writeRawData("fmt ", 4);
    ds << quint32(16);              // PCM fmt chunk is always 16 bytes
    ds << quint16(1);               // audio format: PCM
    ds << channels;
    ds << sampleRate;
    ds << byteRate;
    ds << blockAlign;
    ds << bitsPerSample;

    ds.writeRawData("data", 4);
    ds << dataSize;
    ds.writeRawData(pcm.constData(), static_cast<int>(dataSize));

    return wav;
}
