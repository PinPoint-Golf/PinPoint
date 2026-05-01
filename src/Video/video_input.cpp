#include "video_input.h"

#include <QCamera>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoSink>

VideoInput::VideoInput(QObject *parent)
    : VideoInputBase(parent)
{
}

VideoInput::~VideoInput()
{
    stop();
}

QList<QCameraDevice> VideoInput::availableDevices()
{
    return QMediaDevices::videoInputs();
}

bool VideoInput::start(const QString &deviceId)
{
    stop();

    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    if (devices.isEmpty()) {
        emit errorOccurred(tr("No camera devices available"));
        return false;
    }

    QCameraDevice activeDevice = QMediaDevices::defaultVideoInput();
    if (!deviceId.isEmpty()) {
        for (const QCameraDevice &dev : devices) {
            if (dev.description() == deviceId || dev.id() == deviceId.toUtf8()) {
                activeDevice = dev;
                break;
            }
        }
    }

    m_session = new QMediaCaptureSession(this);
    m_camera  = new QCamera(activeDevice, this);
    m_sink    = new QVideoSink(this);

    m_session->setCamera(m_camera);
    m_session->setVideoSink(m_sink);

    connect(m_camera, &QCamera::activeChanged,
            this, &VideoInput::onCameraActiveChanged);
    connect(m_camera, &QCamera::errorOccurred,
            this, &VideoInput::onCameraErrorOccurred);
    // QVideoSink may emit from any thread; queue to stay on our thread.
    connect(m_sink, &QVideoSink::videoFrameChanged,
            this, &VideoInput::onVideoFrameChanged,
            Qt::QueuedConnection);

    m_camera->start();

    return true;
}

void VideoInput::stop()
{
    if (m_camera) {
        disconnect(m_camera, nullptr, this, nullptr);
        m_camera->stop();
    }
    if (m_sink)
        disconnect(m_sink, nullptr, this, nullptr);

    if (m_session) {
        m_session->setCamera(nullptr);
        m_session->setVideoSink(nullptr);
        delete m_session;
        m_session = nullptr;
    }
    delete m_camera;  m_camera = nullptr;
    delete m_sink;    m_sink   = nullptr;

    if (m_state != State::Stopped) {
        m_state = State::Stopped;
        emit stateChanged(State::Stopped);
    }
}

void VideoInput::suspend()
{
    if (m_camera && m_state == State::Active) {
        m_state = State::Suspended;
        m_camera->stop();
        emit stateChanged(State::Suspended);
    }
}

void VideoInput::resume()
{
    if (m_camera && m_state == State::Suspended)
        m_camera->start();
}

bool VideoInput::isActive() const
{
    return m_state == State::Active;
}

QVideoFrameFormat VideoInput::frameFormat() const
{
    if (m_sink) {
        const QVideoFrameFormat fmt = m_sink->videoFrame().surfaceFormat();
        if (fmt.isValid())
            return fmt;
    }
    return m_preferredFormat;
}

void VideoInput::setPreferredFormat(const QVideoFrameFormat &format)
{
    m_preferredFormat = format;
}

void VideoInput::onCameraActiveChanged(bool active)
{
    if (active) {
        if (m_state != State::Active) {
            m_state = State::Active;
            emit stateChanged(State::Active);
        }
    } else {
        // Don't clobber Suspended or Error — only transition from Active.
        if (m_state == State::Active) {
            m_state = State::Stopped;
            emit stateChanged(State::Stopped);
        }
    }
}

void VideoInput::onCameraErrorOccurred(QCamera::Error error, const QString &errorString)
{
    if (error != QCamera::NoError) {
        m_state = State::Error;
        emit errorOccurred(errorString);
        emit stateChanged(State::Error);
    }
}

void VideoInput::onVideoFrameChanged(const QVideoFrame &frame)
{
    if (frame.isValid())
        emit videoFrameReady(frame);
}
