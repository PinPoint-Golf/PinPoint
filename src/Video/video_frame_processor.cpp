#include "video_frame_processor.h"

VideoFrameProcessor::VideoFrameProcessor(QObject *parent)
    : VideoFrameProcessorBase(parent)
{
}

void VideoFrameProcessor::processFrame(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;
    const QImage image = frame.toImage();
    if (!image.isNull())
        emit frameReady(image);
}
