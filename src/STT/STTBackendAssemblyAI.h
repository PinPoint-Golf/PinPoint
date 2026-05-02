#pragma once
#include "STTBackend.h"
#include <QAbstractSocket>
#include <QString>

class QTimer;
class QWebSocket;

// STT backend that streams audio to AssemblyAI's real-time transcription API.
// Used automatically when no GPU is available for local (whisper.cpp) inference.
// Requires an AssemblyAI API key stored in the "assemblyaiApiKey" secret.
//
// Connection lifecycle (tied to listening state, not inactivity):
//   - Opened lazily on the first transcribe() call after the user starts listening.
//   - Kept open through silence so AssemblyAI retains full utterance context.
//   - Closed gracefully via stopStreaming() when the user stops listening.
//   - A kAbortMs fallback timer force-closes if the server never sends Termination.
class STTBackendAssemblyAI : public STTBackend {
    Q_OBJECT
public:
    explicit STTBackendAssemblyAI(const QString& apiKey, QObject* parent = nullptr);
    ~STTBackendAssemblyAI() override;

    bool    loadModel(const QString& modelPath) override;
    void    transcribe(const std::vector<float>& pcmF32) override;
    void    stopStreaming() override;
    bool    isReady() const override;
    bool    requiresModelFile()    const override { return false; }
    bool    requiresSilenceGating() const override { return false; }
    QString backendLabel() const override { return QStringLiteral("Cloud"); }

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onSocketError(QAbstractSocket::SocketError error);
    void onAbortTimeout();

private:
    void openConnection();
    void beginTerminate();

    // Force-close if the server hasn't sent Termination within this window.
    static constexpr int kAbortMs = 5'000;

    QString     m_apiKey;
    QWebSocket* m_socket = nullptr;
    QTimer*     m_abortTimer = nullptr;
    // Raw transcript for the current turn, kept as a fallback in case AssemblyAI
    // never populates the formatted "utterance" field before end_of_turn.
    QString     m_pendingPartial;
    // True once we have emitted text for the current turn (via utterance or end_of_turn),
    // so we do not emit the same turn twice.
    bool        m_emittedThisTurn = false;
};
