#pragma once
#include "STTBackend.h"
#include <memory>

// macOS-native STT backend using SFSpeechRecognizer (Speech framework).
// No model file is required; recognition is performed on-device (macOS 13+)
// or via Apple's servers on earlier releases.
class STTBackendApple : public STTBackend {
    Q_OBJECT
public:
    explicit STTBackendApple(QObject* parent = nullptr);
    ~STTBackendApple() override;

    // modelPath is ignored — the Speech framework uses its own built-in models.
    // Requests speech recognition authorization from the user on first call.
    bool loadModel(const QString& modelPath) override;
    void transcribe(const std::vector<float>& pcmF32) override;
    bool isReady() const override;
    bool requiresModelFile() const override { return false; }

private:
    struct Private;
    Private* d = nullptr;
};
