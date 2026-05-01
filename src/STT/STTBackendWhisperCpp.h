#pragma once
#include "STTBackend.h"
#include "whisper.h"

class STTBackendWhisperCpp : public STTBackend {
  Q_OBJECT
public:
  explicit STTBackendWhisperCpp(QObject* parent = nullptr);
  ~STTBackendWhisperCpp() override;

  bool loadModel(const QString& modelPath) override;
  void transcribe(const std::vector<float>& pcmF32) override;
  bool isReady() const override;

private:
  whisper_context* m_ctx = nullptr;
};
