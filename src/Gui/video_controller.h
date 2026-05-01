#pragma once

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>

class QThread;
class VideoInputBase;
class VideoFrameProcessor;

// ---------------------------------------------------------------------------
// VideoFrameImageProvider
//
// Thread-safe QQuickImageProvider that serves the latest camera frame.
// Update via updateFrame() (main thread); QML fetches via requestImage()
// (render thread).  Force a re-fetch by appending a changing query string to
// the source URL, e.g. "image://videoframe/frame?t=" + videoController.frameId
// ---------------------------------------------------------------------------

class VideoFrameImageProvider : public QQuickImageProvider
{
public:
    explicit VideoFrameImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    void updateFrame(const QImage &image);

private:
    QMutex m_mutex;
    QImage m_frame;
};

// ---------------------------------------------------------------------------
// VideoController
// ---------------------------------------------------------------------------

class VideoController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isRecording READ isRecording NOTIFY isRecordingChanged)
    Q_PROPERTY(int  frameId     READ frameId     NOTIFY frameIdChanged)

public:
    explicit VideoController(QObject *parent = nullptr);
    ~VideoController() override;

    bool isRecording() const;
    int  frameId()     const;

    // Pass the returned pointer to engine.addImageProvider("videoframe", ...)
    // before engine.loadFromModule().  The QML engine takes ownership.
    QQuickImageProvider *imageProvider() const;

    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

signals:
    void isRecordingChanged();
    void frameIdChanged();

private slots:
    void onFrameReady(const QImage &image);
    void onVideoError(const QString &message);
    void startCapture();

private:
    QThread                 *m_captureThread   = nullptr;
    QThread                 *m_processorThread = nullptr;
    VideoInputBase          *m_videoInput      = nullptr;
    VideoFrameProcessor     *m_processor       = nullptr;
    VideoFrameImageProvider *m_imageProvider   = nullptr;  // owned by QML engine
    bool m_recording = false;
    int  m_frameId   = 0;
};
