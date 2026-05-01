#include "video_input_base.h"
#include "video_frame_processor_base.h"

VideoInputBase::VideoInputBase(QObject *parent)
    : QObject(parent)
{
}

VideoInputBase::State VideoInputBase::state() const
{
    return m_state;
}

void VideoInputBase::connectProcessor(VideoFrameProcessorBase *processor)
{
    connect(this, &VideoInputBase::videoFrameReady,
            processor, &VideoFrameProcessorBase::processFrame,
            Qt::QueuedConnection);
}
