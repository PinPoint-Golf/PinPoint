#include "video_controller.h"

#include "video_frame_processor.h"
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

#ifdef Q_OS_MACOS
#include "macos_permissions.h"
#endif

// ---------------------------------------------------------------------------
// VideoFrameImageProvider
// ---------------------------------------------------------------------------

VideoFrameImageProvider::VideoFrameImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage VideoFrameImageProvider::requestImage(const QString & /*id*/,
                                             QSize *size,
                                             const QSize &requestedSize)
{
    QMutexLocker lock(&m_mutex);
    QImage result = m_frame;
    lock.unlock();

    if (result.isNull()) {
        // Return a 1×1 transparent placeholder so QML doesn't log a "failed to
        // get image" warning before the first real frame has arrived.
        result = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        if (size)
            *size = result.size();
        return result;
    }

    if (size)
        *size = result.size();

    if (!requestedSize.isEmpty())
        result = result.scaled(requestedSize, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    return result;
}

void VideoFrameImageProvider::updateFrame(const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    m_frame = image;
}

// ---------------------------------------------------------------------------
// VideoController
// ---------------------------------------------------------------------------

VideoController::VideoController(QObject *parent)
    : QObject(parent)
    , m_captureThread(new QThread(this))
    , m_processorThread(new QThread(this))
#ifdef Q_OS_MACOS
    , m_videoInput(new VideoInputApple)
#else
    , m_videoInput(new VideoInput)
#endif
    , m_processor(new VideoFrameProcessor)
    , m_imageProvider(new VideoFrameImageProvider)
{
    m_captureThread->setObjectName(QStringLiteral("VideoCaptureThread"));
    m_processorThread->setObjectName(QStringLiteral("VideoProcessorThread"));

    m_videoInput->moveToThread(m_captureThread);
    m_processor->moveToThread(m_processorThread);

    m_videoInput->connectProcessor(m_processor);

    connect(m_processor, &VideoFrameProcessor::frameReady,
            this, &VideoController::onFrameReady);
    connect(m_videoInput, &VideoInputBase::errorOccurred,
            this, &VideoController::onVideoError);

    // Start threads immediately.  The actual camera device is only opened in
    // startRecording(), where AVFoundation permission is checked first.
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

    if (m_processorThread->isRunning()) {
        QMetaObject::invokeMethod(m_processor, [this]() {
            m_processor->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);

        m_processorThread->quit();
        m_processorThread->wait();
    }
    delete m_processor;
    m_processor = nullptr;

    // m_imageProvider is owned by the QML engine — do not delete.
}

bool VideoController::isRecording() const
{
    return m_recording;
}

int VideoController::frameId() const
{
    return m_frameId;
}

QQuickImageProvider *VideoController::imageProvider() const
{
    return m_imageProvider;
}

void VideoController::startRecording()
{
    if (m_recording || !m_captureThread->isRunning())
        return;

#ifdef Q_OS_MACOS
    // Check / request AVFoundation camera permission immediately before the
    // camera is opened.  This shows the system dialog in context (user just
    // clicked Start) and avoids the spurious Qt permission-plugin warnings that
    // appear when QCameraPermission is invoked before Qt's multimedia backend
    // has fully initialised.
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

void VideoController::onFrameReady(const QImage &image)
{
    m_imageProvider->updateFrame(image);
    ++m_frameId;
    emit frameIdChanged();
}

void VideoController::onVideoError(const QString &message)
{
    qWarning() << "[Video]" << message;
}

void VideoController::startCapture()
{
    m_captureThread->start();
    m_processorThread->start();
}
