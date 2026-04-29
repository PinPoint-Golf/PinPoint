#include "audio_output_base.h"
#include "audio_input_base.h"

AudioOutputBase::AudioOutputBase(QObject *parent)
    : QObject(parent)
{
}

AudioOutputBase::State AudioOutputBase::state() const
{
    return m_state;
}

void AudioOutputBase::connectSource(AudioInputBase *source)
{
    connect(source, &AudioInputBase::audioDataReady,
            this, &AudioOutputBase::writeAudio,
            Qt::QueuedConnection);
}
