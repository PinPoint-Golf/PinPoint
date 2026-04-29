#pragma once
#include <QObject>
#include <QString>
#include <vector>

class WhisperBackend : public QObject {
  Q_OBJECT
public:
  explicit WhisperBackend(QObject* parent = nullptr) : QObject(parent) {}
  virtual ~WhisperBackend() = default;

  // Load model from an absolute file path. Returns true on success.
  virtual bool loadModel(const QString& modelPath) = 0;

  // Transcribe audio. Input MUST be 16kHz, mono, 32-bit float PCM.
  // Result delivered via transcriptionReady signal.
  virtual void transcribe(const std::vector<float>& pcmF32) = 0;

  virtual bool isReady() const = 0;

signals:
  void transcriptionReady(const QString& text);
  void transcriptionFailed(const QString& error);
};
