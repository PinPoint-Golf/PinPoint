#include "STTBackendWhisperCpp.h"
#include "ggml-backend.h"
#include <QDebug>
#include <cstring>

STTBackendWhisperCpp::STTBackendWhisperCpp(QObject* parent)
  : STTBackend(parent) {}

STTBackendWhisperCpp::~STTBackendWhisperCpp() {
  if (m_ctx) {
    whisper_free(m_ctx);
    m_ctx = nullptr;
  }
}

bool STTBackendWhisperCpp::loadModel(const QString& modelPath) {
  if (m_ctx) { whisper_free(m_ctx); m_ctx = nullptr; }
  // toLocal8Bit() gives the correct narrow path string on all three platforms
  // for typical (ASCII/Latin) paths. For non-ASCII Windows paths, a future
  // improvement would use a wide-string variant if whisper.cpp adds one.
  whisper_context_params cparams = whisper_context_default_params();
  m_ctx = whisper_init_from_file_with_params(modelPath.toLocal8Bit().constData(), cparams);
  if (!m_ctx) {
    qWarning() << "[STTBackendWhisperCpp] Failed to load model from" << modelPath;
    return false;
  }
  return true;
}

void STTBackendWhisperCpp::transcribe(const std::vector<float>& pcmF32) {
  if (!m_ctx) { emit transcriptionFailed("Model not loaded"); return; }
  if (pcmF32.empty()) { emit transcriptionFailed("Empty audio buffer"); return; }

  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.language         = "en";
  params.translate        = false;
  params.no_context       = true;
  params.single_segment   = false;
  params.print_realtime   = false;
  params.print_progress   = false;
  params.print_timestamps = false;

  int result = whisper_full(m_ctx, params,
                            pcmF32.data(),
                            static_cast<int>(pcmF32.size()));
  if (result != 0) {
    emit transcriptionFailed(
        QString("whisper_full failed with code %1").arg(result));
    return;
  }

  QString text;
  const int nSegments = whisper_full_n_segments(m_ctx);
  for (int i = 0; i < nSegments; ++i)
    text += QString::fromUtf8(whisper_full_get_segment_text(m_ctx, i));

  emit transcriptionReady(text.trimmed());
}

bool STTBackendWhisperCpp::isReady() const { return m_ctx != nullptr; }

QString STTBackendWhisperCpp::backendLabel() const {
    // Walk every ggml backend registered at library load time.
    // GPU backends (Vulkan, CUDA, …) report zero devices when no suitable
    // hardware or driver is present, which is how we detect a silent CPU fallback.
    QStringList gpu;
    const size_t n = ggml_backend_reg_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_reg_t reg = ggml_backend_reg_get(i);
        const char *name = ggml_backend_reg_name(reg);
        if (strcmp(name, "CPU") == 0 || strcmp(name, "BLAS") == 0)
            continue;
        if (ggml_backend_reg_dev_count(reg) > 0)
            gpu << QString::fromUtf8(name);
    }
    return gpu.isEmpty() ? QStringLiteral("CPU") : gpu.join('+');
}
