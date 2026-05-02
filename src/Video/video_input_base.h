#pragma once

#include <QObject>
#include <QVideoFrameFormat>

// Abstract base for camera / video capture.
//
// Subclasses implement the transport (Qt6 QCamera, platform-specific, etc.)
// and emit videoFrameReady() whenever a decoded frame arrives.
//
// Typical usage:
//   VideoInput *in = new VideoInput(this);
//   in->start();                           // default camera
//   in->start("Front Camera");             // named device

class VideoInputBase : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Active,
        Suspended,
        Error,
    };
    Q_ENUM(State)

    explicit VideoInputBase(QObject *parent = nullptr);
    ~VideoInputBase() override = default;

    // -----------------------------------------------------------------------
    // Transport control
    // -----------------------------------------------------------------------

    // Start capture on the named device; empty string selects the system default.
    // deviceId is matched against QCameraDevice::id() (byte string) or description().
    virtual bool start(const QString &deviceId = {}) = 0;
    virtual void stop() = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;

    // -----------------------------------------------------------------------
    // Status
    // -----------------------------------------------------------------------

    virtual bool              isActive()    const = 0;
    virtual QVideoFrameFormat frameFormat() const = 0;
    virtual State             state()       const;

signals:
    // Emitted for every decoded camera frame.
    void videoFrameReady(const QVideoFrame &frame);

    void stateChanged(VideoInputBase::State state);
    void errorOccurred(const QString &message);

protected:
    State m_state = State::Stopped;
};
