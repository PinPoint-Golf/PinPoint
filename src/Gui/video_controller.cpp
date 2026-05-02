#include "video_controller.h"

#include "video_input_base.h"

#ifdef Q_OS_MACOS
#include "VideoInputApple.h"
#else
#include "video_input.h"
#endif

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QVideoFrame>
#include <QVideoSink>

#ifdef Q_OS_MACOS
#include "macos_permissions.h"
#endif

VideoController::VideoController(QObject *parent)
    : QObject(parent)
    , m_captureThread(new QThread(this))
#ifdef Q_OS_MACOS
    , m_videoInput(new VideoInputApple)
#else
    , m_videoInput(new VideoInput)
#endif
{
    m_captureThread->setObjectName(QStringLiteral("VideoCaptureThread"));
    m_videoInput->moveToThread(m_captureThread);

    connect(m_videoInput, &VideoInputBase::videoFrameReady,
            this, &VideoController::onVideoFrame,
            Qt::QueuedConnection);
    connect(m_videoInput, &VideoInputBase::errorOccurred,
            this, &VideoController::onVideoError);

    startCapture();
}

VideoController::~VideoController()
{
    if (m_captureThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoInput, [this]() {
            m_videoInput->stop();
            m_videoInput->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);

        m_captureThread->quit();
        m_captureThread->wait();
    }
    delete m_videoInput;
    m_videoInput = nullptr;
}

bool VideoController::isRecording() const
{
    return m_recording;
}

void VideoController::setVideoSink(QVideoSink *sink)
{
    m_videoSink = sink;
}

void VideoController::startRecording()
{
    if (m_recording || !m_captureThread->isRunning())
        return;

#ifdef Q_OS_MACOS
    auto *self = this;
    requestCameraPermission([self](bool granted) {
        QMetaObject::invokeMethod(self, [self, granted]() {
            if (!granted) {
                qWarning() << "[VideoController] Camera permission denied."
                           << "Grant access in System Settings → Privacy & Security → Camera.";
                return;
            }
            QMetaObject::invokeMethod(self->m_videoInput, [self]() {
                self->m_videoInput->start();
            }, Qt::QueuedConnection);
            self->m_recording = true;
            emit self->isRecordingChanged();
        }, Qt::QueuedConnection);
    });
#else
    QMetaObject::invokeMethod(m_videoInput, [this]() {
        m_videoInput->start();
    }, Qt::QueuedConnection);
    m_recording = true;
    emit isRecordingChanged();
#endif
}

void VideoController::stopRecording()
{
    if (!m_recording)
        return;
    QMetaObject::invokeMethod(m_videoInput, [this]() {
        m_videoInput->stop();
    }, Qt::QueuedConnection);
    m_recording = false;
    emit isRecordingChanged();
}

void VideoController::onVideoFrame(const QVideoFrame &frame)
{
    if (m_videoSink && frame.isValid())
        m_videoSink->setVideoFrame(frame);
}

void VideoController::onVideoError(const QString &message)
{
    qWarning() << "[Video]" << message;
}

void VideoController::startCapture()
{
    m_captureThread->start();
}
