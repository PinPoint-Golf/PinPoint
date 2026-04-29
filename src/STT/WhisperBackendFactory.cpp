#include "WhisperBackendFactory.h"
#include "WhisperBackendWhisperCpp.h"

std::unique_ptr<WhisperBackend> WhisperBackendFactory::create(
    Backend backend, QObject* parent)
{
  switch (backend) {
    case Backend::WhisperCpp:
      return std::make_unique<WhisperBackendWhisperCpp>(parent);
  }
  return nullptr;
}
