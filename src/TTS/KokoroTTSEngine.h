#pragma once
#include "TTSEngine.h"
#include "PhonemeTokenizer.h"

#include <atomic>
#include <memory>
#include <vector>
#include <QString>

// Kokoro TTS engine backed by ONNX Runtime.
//
// Expected model I/O (verify against the actual graph with Netron or
//   python -c "import onnx; m=onnx.load('kokoro.onnx');
//              [print(t.name,t.type) for t in m.graph.input+m.graph.output]"
// before deploying):
//
//   Input  "input_ids" : int64  [1, sequence_length] — token ID sequence
//   Input  "style"     : float  [1, 256]             — voice style vector
//   Input  "speed"     : float  [1]                  — playback speed
//   Output "waveform"  : float  [1, num_samples]     — 24 kHz mono float32 PCM
//
// Build note:
//   Fully functional only when built with -DHAVE_ONNXRUNTIME (set automatically
//   by CMake when ONNXRUNTIME_ROOT is provided).  Without it the class is
//   creatable and loadModel()/synthesise() immediately emit errorOccurred().

class KokoroTTSEngine : public TTSEngine
{
    Q_OBJECT

public:
    explicit KokoroTTSEngine(QObject *parent = nullptr);
    ~KokoroTTSEngine() override;

    bool loadModel(const QString &modelPath,
                   const QString &voicePath,
                   const QString &tokensPath) override;

    void synthesise(const QString &text) override;
    void stop() override;
    bool isReady() const override;

    QString gpuBackend() const override { return m_gpuBackend; }

private:
    static QAudioFormat kokoroFormat();

    PhonemeTokenizer   m_tokenizer;
    std::vector<float> m_styleVec;          // 256-element packed float32 style vector
    bool               m_ready = false;
    std::atomic<bool>  m_stopFlag { false };
    QString            m_gpuBackend;        // empty = CPU; "CoreML", "CUDA", or "DirectML" when GPU active

#ifdef HAVE_ONNXRUNTIME
    struct OrtState;
    std::unique_ptr<OrtState> m_ort;
#endif
};
