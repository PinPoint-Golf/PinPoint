#include "WhisperBackendWhisperCpp.h"
#include <QDebug>

WhisperBackendWhisperCpp::WhisperBackendWhisperCpp(QObject* parent)
  : WhisperBackend(parent) {}

WhisperBackendWhisperCpp::~WhisperBackendWhisperCpp() {
  if (m_ctx) {
    whisper_free(m_ctx);
    m_ctx = nullptr;
  }
}

bool WhisperBackendWhisperCpp::loadModel(const QString& modelPath) {
  if (m_ctx) { whisper_free(m_ctx); m_ctx = nullptr; }
  // toLocal8Bit() gives the correct narrow path string on all three platforms
  // for typical (ASCII/Latin) paths. For non-ASCII Windows paths, a future
  // improvement would use a wide-string variant if whisper.cpp adds one.
  whisper_context_params cparams = whisper_context_default_params();
  m_ctx = whisper_init_from_file_with_params(modelPath.toLocal8Bit().constData(), cparams);
  if (!m_ctx) {
    qWarning() << "WhisperBackend: failed to load model from" << modelPath;
    return false;
  }
  return true;
}

void WhisperBackendWhisperCpp::transcribe(const std::vector<float>& pcmF32) {
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

bool WhisperBackendWhisperCpp::isReady() const { return m_ctx != nullptr; }
