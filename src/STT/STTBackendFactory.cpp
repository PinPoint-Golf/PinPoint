#include "STTBackendFactory.h"
#include "STTBackendWhisperCpp.h"
#if defined(Q_OS_MACOS)
#  include "STTBackendApple.h"
#endif
#include <QDebug>

std::unique_ptr<STTBackend> STTBackendFactory::create(
    Backend backend, QObject* parent)
{
  switch (backend) {
    case Backend::WhisperCpp:
      return std::make_unique<STTBackendWhisperCpp>(parent);
    case Backend::Apple:
#if defined(Q_OS_MACOS)
      return std::make_unique<STTBackendApple>(parent);
#else
      qWarning("STTBackendFactory: Apple backend requested on non-macOS platform; "
               "falling back to WhisperCpp");
      return std::make_unique<STTBackendWhisperCpp>(parent);
#endif
  }
  return nullptr;
}

std::unique_ptr<STTBackend> STTBackendFactory::createDefault(QObject* parent)
{
#if defined(Q_OS_MACOS)
  return create(Backend::Apple, parent);
#else
  return create(Backend::WhisperCpp, parent);
#endif
}
