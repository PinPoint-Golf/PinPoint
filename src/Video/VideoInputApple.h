#pragma once

#include "video_input_base.h"

// Private implementation — defined only in VideoInputApple.mm so that
// Objective-C types never leak into C++ translation units.
struct VideoInputApplePrivate;

// macOS-native camera backend using AVCaptureSession / AVCaptureVideoDataOutput.
//
// Completely bypasses Qt's QCamera so that Qt's QCameraPermission plugin
// (which is absent in the FFmpeg multimedia build) is never invoked.
// Frames arrive as BGRA CVPixelBuffers and are wrapped in a QVideoFrame
// before being emitted on videoFrameReady().
//
// AVFoundation permission must be obtained via requestCameraPermission()
// (macos_permissions.h) before calling start().

class VideoInputApple : public VideoInputBase
{
    Q_OBJECT

public:
    explicit VideoInputApple(QObject *parent = nullptr);
    ~VideoInputApple() override;

    bool              start(const QString &deviceId = {}) override;
    void              stop()    override;
    void              suspend() override;
    void              resume()  override;
    bool              isActive()    const override;
    QVideoFrameFormat frameFormat() const override;

    // Called by the Obj-C sample-buffer delegate — not for external use.
    void onFrameCaptured(const QVideoFrame &frame);

private:
    VideoInputApplePrivate *d = nullptr;
};
