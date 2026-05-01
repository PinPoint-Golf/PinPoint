#pragma once
#include "STTBackend.h"
#include <memory>

class STTBackendFactory {
public:
  enum class Backend {
    WhisperCpp, // whisper.cpp (all platforms)
    Apple,      // SFSpeechRecognizer — macOS only; falls back to WhisperCpp elsewhere
  };

  static std::unique_ptr<STTBackend> create(
      Backend backend,
      QObject* parent = nullptr);

  // Returns the best available backend for the current platform.
  static std::unique_ptr<STTBackend> createDefault(QObject* parent = nullptr);
};
