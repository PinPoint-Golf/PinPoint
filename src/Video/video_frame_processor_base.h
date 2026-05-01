#pragma once

#include <QObject>
#include <QVideoFrame>

// Abstract base for classes that consume a stream of video frames.
//
// Subclasses override processFrame() and are connected to a VideoInputBase
// via VideoInputBase::connectProcessor(), or manually via:
//   connect(input, &VideoInputBase::videoFrameReady,
//           processor, &VideoFrameProcessorBase::processFrame);
//
// processFrame() is called with whatever frames the platform delivers;
// subclasses must not assume a fixed resolution or pixel format.

class VideoFrameProcessorBase : public QObject
{
    Q_OBJECT

public:
    explicit VideoFrameProcessorBase(QObject *parent = nullptr);
    ~VideoFrameProcessorBase() override = default;

public slots:
    virtual void processFrame(const QVideoFrame &frame) = 0;
};
