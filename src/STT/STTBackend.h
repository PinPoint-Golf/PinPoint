#pragma once
#include <QObject>
#include <QString>
#include <vector>

class STTBackend : public QObject {
  Q_OBJECT
public:
  explicit STTBackend(QObject* parent = nullptr) : QObject(parent) {}
  virtual ~STTBackend() = default;

  // Load model from an absolute file path. Returns true on success.
  virtual bool loadModel(const QString& modelPath) = 0;

  // Transcribe audio. Input MUST be 16kHz, mono, 32-bit float PCM.
  // Result delivered via transcriptionReady signal.
  virtual void transcribe(const std::vector<float>& pcmF32) = 0;

  virtual bool isReady() const = 0;

  // Returns true if this backend needs a local model file passed to loadModel().
  // Backends that use OS-provided models (e.g. Apple Speech) return false.
  virtual bool requiresModelFile() const { return true; }

signals:
  void transcriptionReady(const QString& text);
  void transcriptionFailed(const QString& error);
};
