#pragma once
#include "WhisperBackend.h"
#include "whisper.h"

class WhisperBackendWhisperCpp : public WhisperBackend {
  Q_OBJECT
public:
  explicit WhisperBackendWhisperCpp(QObject* parent = nullptr);
  ~WhisperBackendWhisperCpp() override;

  bool loadModel(const QString& modelPath) override;
  void transcribe(const std::vector<float>& pcmF32) override;
  bool isReady() const override;

private:
  whisper_context* m_ctx = nullptr;
};
