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

  // Returns true if silent chunks should be dropped before calling transcribe().
  // Local backends (whisper.cpp) benefit from this — no wasted inference on silence.
  // Cloud backends (AssemblyAI) need silence delivered so the server can detect
  // end-of-turn and emit formatted utterances without waiting for Terminate.
  virtual bool requiresSilenceGating() const { return true; }

  // Short label describing the compute backend, e.g. "CPU", "Vulkan", "CUDA", "Apple".
  virtual QString backendLabel() const { return QStringLiteral("CPU"); }

  // Called when the user stops listening. Cloud backends should close their
  // connection here; local backends can ignore it.
  virtual void stopStreaming() {}

signals:
  void transcriptionReady(const QString& text);
  void transcriptionFailed(const QString& error);
};
