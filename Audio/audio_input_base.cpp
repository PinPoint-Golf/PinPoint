#include "audio_input_base.h"
#include "audio_processor_base.h"

AudioInputBase::AudioInputBase(QObject *parent)
    : QObject(parent)
{
}

AudioInputBase::State AudioInputBase::state() const
{
    return m_state;
}

void AudioInputBase::connectProcessor(AudioProcessorBase *processor)
{
    connect(this, &AudioInputBase::audioDataReady,
            processor, &AudioProcessorBase::processAudio,
            Qt::QueuedConnection);
}
