#pragma once

#include "video_frame_processor_base.h"
#include <QImage>

// Concrete VideoFrameProcessorBase that converts each arriving QVideoFrame
// to a QImage and emits it.  Intended to run on a dedicated thread via
// VideoInputBase::connectProcessor() + moveToThread().

class VideoFrameProcessor : public VideoFrameProcessorBase
{
    Q_OBJECT

public:
    explicit VideoFrameProcessor(QObject *parent = nullptr);

public slots:
    void processFrame(const QVideoFrame &frame) override;

signals:
    void frameReady(const QImage &image);
};
