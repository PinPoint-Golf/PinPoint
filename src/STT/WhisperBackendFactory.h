#pragma once
#include "WhisperBackend.h"
#include <memory>

class WhisperBackendFactory {
public:
  enum class Backend { WhisperCpp };

  static std::unique_ptr<WhisperBackend> create(
      Backend backend = Backend::WhisperCpp,
      QObject* parent = nullptr);
};
