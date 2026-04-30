#include "TTSEngineFactory.h"
#include "KokoroTTSEngine.h"

std::unique_ptr<TTSEngine> TTSEngineFactory::create(Backend backend, QObject *parent)
{
    switch (backend) {
    case Backend::Kokoro:
        return std::make_unique<KokoroTTSEngine>(parent);
    }
    return nullptr;
}
