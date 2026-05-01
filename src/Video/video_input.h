#pragma once

#include "video_input_base.h"
#include <QCamera>
#include <QCameraDevice>
#include <QVideoFrameFormat>

class QMediaCaptureSession;
class QVideoSink;

// Qt6 Multimedia implementation of VideoInputBase.
//
// Wires a QCamera into a QMediaCaptureSession with a QVideoSink that
// forwards every decoded frame into the videoFrameReady signal.
//
// Device selection:
//   Pass an empty string (default) to use QMediaDevices::defaultVideoInput().
//   Pass a description string to match against QCameraDevice::description(),
//   or a device-id string to match against QCameraDevice::id().
//   Use VideoInput::availableDevices() to enumerate candidates.

class VideoInput : public VideoInputBase
{
    Q_OBJECT

public:
    explicit VideoInput(QObject *parent = nullptr);
    ~VideoInput() override;

    // Returns every available camera on this platform.
    static QList<QCameraDevice> availableDevices();

    // -----------------------------------------------------------------------
    // VideoInputBase interface
    // -----------------------------------------------------------------------

    bool              start(const QString &deviceId = {}) override;
    void              stop()    override;
    void              suspend() override;
    void              resume()  override;
    bool              isActive()    const override;
    QVideoFrameFormat frameFormat() const override;

    // -----------------------------------------------------------------------
    // Configuration (call before start())
    // -----------------------------------------------------------------------

    // Hint for resolution / pixel format; the camera may ignore it.
    void setPreferredFormat(const QVideoFrameFormat &format);

private slots:
    void onCameraActiveChanged(bool active);
    void onCameraErrorOccurred(QCamera::Error error, const QString &errorString);
    void onVideoFrameChanged(const QVideoFrame &frame);

private:
    QCamera              *m_camera  = nullptr;
    QMediaCaptureSession *m_session = nullptr;
    QVideoSink           *m_sink    = nullptr;
    QVideoFrameFormat     m_preferredFormat;
};
