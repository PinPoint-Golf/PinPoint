#pragma once
#include "TTSEngine.h"
#include <memory>

class TTSEngineFactory
{
public:
    enum class Backend { Kokoro };

    static std::unique_ptr<TTSEngine> create(Backend backend = Backend::Kokoro,
                                             QObject *parent = nullptr);
};
