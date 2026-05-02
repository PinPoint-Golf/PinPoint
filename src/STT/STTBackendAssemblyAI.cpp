#include "STTBackendAssemblyAI.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <algorithm>
#include <cstdint>

static const char* kStreamingUrl =
    "wss://streaming.assemblyai.com/v3/ws"
    "?sample_rate=16000&encoding=pcm_s16le&speech_model=universal-streaming-english";

STTBackendAssemblyAI::STTBackendAssemblyAI(const QString& apiKey, QObject* parent)
    : STTBackend(parent)
    , m_apiKey(apiKey)
{}

STTBackendAssemblyAI::~STTBackendAssemblyAI()
{
    if (m_abortTimer) m_abortTimer->stop();
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->sendTextMessage(QStringLiteral("{\"type\":\"Terminate\"}"));
        m_socket->close();
    }
}

bool STTBackendAssemblyAI::loadModel(const QString&)
{
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_socket, &QWebSocket::connected,
            this, &STTBackendAssemblyAI::onConnected);
    connect(m_socket, &QWebSocket::disconnected,
            this, &STTBackendAssemblyAI::onDisconnected);
    connect(m_socket, &QWebSocket::textMessageReceived,
            this, &STTBackendAssemblyAI::onTextMessageReceived);
    connect(m_socket, &QWebSocket::errorOccurred,
            this, &STTBackendAssemblyAI::onSocketError);
    connect(m_socket, &QWebSocket::sslErrors,
            this, [this](const QList<QSslError> &errors) {
                for (const QSslError &e : errors)
                    qWarning() << "[AssemblyAI] SSL error:" << e.errorString();
            });

    m_abortTimer = new QTimer(this);
    m_abortTimer->setSingleShot(true);
    m_abortTimer->setInterval(kAbortMs);
    connect(m_abortTimer, &QTimer::timeout,
            this, &STTBackendAssemblyAI::onAbortTimeout);

    return true;
}

void STTBackendAssemblyAI::openConnection()
{
    QNetworkRequest req{QUrl(QString::fromLatin1(kStreamingUrl))};
    req.setRawHeader("Authorization", m_apiKey.toLatin1());
    m_socket->open(req);
}

void STTBackendAssemblyAI::beginTerminate()
{
    if (!m_socket) return;
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->sendTextMessage(QStringLiteral("{\"type\":\"Terminate\"}"));
        m_abortTimer->start();
    } else if (m_socket->state() == QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }
}

void STTBackendAssemblyAI::transcribe(const std::vector<float>& pcmF32)
{
    if (!m_socket) return;

    switch (m_socket->state()) {
        case QAbstractSocket::UnconnectedState:
            openConnection();
            return;
        case QAbstractSocket::ConnectedState:
            break;
        default:
            return;
    }

    // AssemblyAI requires chunks between 50 ms and 1000 ms.
    // At 16 kHz that is 800–16 000 samples. Use 960 ms (15 360 samples) per
    // message so a 3-second buffer from STTProcessor sends as 3–4 messages.
    static constexpr size_t kMaxSamples = 15'360;

    size_t offset = 0;
    while (offset < pcmF32.size()) {
        const size_t count = std::min(kMaxSamples, pcmF32.size() - offset);
        QByteArray pcm16;
        pcm16.resize(static_cast<qsizetype>(count * 2));
        auto* out = reinterpret_cast<int16_t*>(pcm16.data());
        for (size_t i = 0; i < count; ++i) {
            const float s = std::max(-1.0f, std::min(1.0f, pcmF32[offset + i]));
            out[i] = static_cast<int16_t>(s * 32767.0f);
        }
        m_socket->sendBinaryMessage(pcm16);
        offset += count;
    }
}

void STTBackendAssemblyAI::stopStreaming()
{
    beginTerminate();
}

bool STTBackendAssemblyAI::isReady() const
{
    return m_socket != nullptr;
}

void STTBackendAssemblyAI::onConnected()  {}

void STTBackendAssemblyAI::onDisconnected()
{
    m_abortTimer->stop();
    m_pendingPartial.clear();
    m_emittedThisTurn = false;
}

void STTBackendAssemblyAI::onAbortTimeout()
{
    qWarning() << "[AssemblyAI] No Termination reply — force closing";
    if (!m_pendingPartial.isEmpty()) {
        emit transcriptionReady(m_pendingPartial);
        m_pendingPartial.clear();
    }
    m_socket->close();
}

void STTBackendAssemblyAI::onTextMessageReceived(const QString& message)
{
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;

    const QJsonObject obj = doc.object();
    const QString type = obj[QLatin1String("type")].toString();

    if (type == QLatin1String("Begin"))
        return;

    if (type == QLatin1String("Turn")) {
        // "utterance" carries the punctuated/capitalised form and is populated in a
        // *partial* turn before end_of_turn fires. When end_of_turn arrives, utterance
        // is empty and only the raw lowercase transcript is present. So we emit as
        // soon as utterance first becomes non-empty rather than waiting for end_of_turn.
        const QString utterance  = obj[QLatin1String("utterance")].toString().trimmed();
        const QString transcript = obj[QLatin1String("transcript")].toString().trimmed();
        const bool    endOfTurn  = obj[QLatin1String("end_of_turn")].toBool();

        if (!utterance.isEmpty() && !m_emittedThisTurn) {
            emit transcriptionReady(utterance);
            m_emittedThisTurn = true;
            m_pendingPartial.clear();
        } else if (!m_emittedThisTurn && !transcript.isEmpty()) {
            m_pendingPartial = transcript;
        }

        if (endOfTurn) {
            if (!m_emittedThisTurn && !m_pendingPartial.isEmpty())
                emit transcriptionReady(m_pendingPartial);
            m_pendingPartial.clear();
            m_emittedThisTurn = false;
        }
        return;
    }

    if (type == QLatin1String("Termination")) {
        m_abortTimer->stop();
        if (!m_emittedThisTurn && !m_pendingPartial.isEmpty())
            emit transcriptionReady(m_pendingPartial);
        m_pendingPartial.clear();
        m_emittedThisTurn = false;
        m_socket->close();
        return;
    }

    if (type == QLatin1String("Error")) {
        qWarning() << "[AssemblyAI] Server error:" << obj[QLatin1String("error")].toString();
        return;
    }

    // Fallback: v2-style FinalTranscript
    if (obj[QLatin1String("message_type")].toString() == QLatin1String("FinalTranscript")) {
        const QString text = obj[QLatin1String("text")].toString().trimmed();
        if (!text.isEmpty())
            emit transcriptionReady(text);
    }
}

void STTBackendAssemblyAI::onSocketError(QAbstractSocket::SocketError errorCode)
{
    qWarning() << "[AssemblyAI] Socket error" << static_cast<int>(errorCode)
               << "(" << m_socket->errorString() << ")";
    m_abortTimer->stop();
    m_pendingPartial.clear();
    m_socket->abort();
}
