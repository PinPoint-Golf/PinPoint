#import <AVFoundation/AVFoundation.h>

#include "VideoInputApple.h"

#include <QImage>
#include <QVideoFrame>
#include <QVideoFrameFormat>

// ---------------------------------------------------------------------------
// Private implementation struct (Obj-C strong pointers — ARC manages them)
// ---------------------------------------------------------------------------

struct VideoInputApplePrivate {
    AVCaptureSession             *session  = nil;
    AVCaptureVideoDataOutput     *output   = nil;
    // The delegate is declared below; forward-declare with id here.
    id                            delegate = nil;
};

// ---------------------------------------------------------------------------
// AVCaptureVideoDataOutputSampleBufferDelegate
// ---------------------------------------------------------------------------

@interface VideoInputAppleDelegate
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
// Atomic so the C++ side can nil it from stop() while the capture queue
// may still be mid-callback.
@property (atomic, assign) VideoInputApple *owner;
@end

@implementation VideoInputAppleDelegate

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
    Q_UNUSED(output)
    Q_UNUSED(connection)

    VideoInputApple *owner = self.owner;
    if (!owner)
        return;

    CVImageBufferRef buf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!buf)
        return;

    CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    const uchar *data = static_cast<const uchar *>(CVPixelBufferGetBaseAddress(buf));
    const int width   = static_cast<int>(CVPixelBufferGetWidth(buf));
    const int height  = static_cast<int>(CVPixelBufferGetHeight(buf));
    const int stride  = static_cast<int>(CVPixelBufferGetBytesPerRow(buf));

    // kCVPixelFormatType_32BGRA: byte order B-G-R-A → QImage::Format_ARGB32
    // on little-endian (all Apple Silicon + Intel Macs).
    QImage copy = QImage(data, width, height, stride, QImage::Format_ARGB32).copy();

    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    owner->onFrameCaptured(QVideoFrame(copy));
}

@end

// ---------------------------------------------------------------------------
// VideoInputApple
// ---------------------------------------------------------------------------

VideoInputApple::VideoInputApple(QObject *parent)
    : VideoInputBase(parent)
    , d(new VideoInputApplePrivate)
{
}

VideoInputApple::~VideoInputApple()
{
    stop();
    delete d;
}

bool VideoInputApple::start(const QString &deviceId)
{
    stop();

    @autoreleasepool {
        // ── Find device ────────────────────────────────────────────────────
        AVCaptureDevice *device = nil;

        if (deviceId.isEmpty()) {
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        } else {
            NSString *needle = deviceId.toNSString();
            NSArray<AVCaptureDeviceType> *types = @[
                AVCaptureDeviceTypeBuiltInWideAngleCamera,
                AVCaptureDeviceTypeExternal
            ];
            AVCaptureDeviceDiscoverySession *disco =
                [AVCaptureDeviceDiscoverySession
                    discoverySessionWithDeviceTypes:types
                                          mediaType:AVMediaTypeVideo
                                           position:AVCaptureDevicePositionUnspecified];
            for (AVCaptureDevice *dev in disco.devices) {
                if ([dev.uniqueID isEqualToString:needle] ||
                    [dev.localizedName isEqualToString:needle]) {
                    device = dev;
                    break;
                }
            }
        }

        if (!device) {
            emit errorOccurred(tr("No camera device available"));
            return false;
        }

        // ── Build session ──────────────────────────────────────────────────
        AVCaptureSession *session = [[AVCaptureSession alloc] init];
        session.sessionPreset = AVCaptureSessionPresetHigh;

        NSError *err = nil;
        AVCaptureDeviceInput *input =
            [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
        if (!input) {
            emit errorOccurred(tr("Cannot open camera: %1")
                .arg(QString::fromNSString(err.localizedDescription)));
            return false;
        }
        if (![session canAddInput:input]) {
            emit errorOccurred(tr("Cannot add camera input to capture session"));
            return false;
        }
        [session addInput:input];

        // ── Video output ───────────────────────────────────────────────────
        AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
        output.alwaysDiscardsLateVideoFrames = YES;
        output.videoSettings = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
        };

        VideoInputAppleDelegate *delegate = [[VideoInputAppleDelegate alloc] init];
        delegate.owner = this;
        [output setSampleBufferDelegate:delegate
                                  queue:dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0)];

        if (![session canAddOutput:output]) {
            delegate.owner = nullptr;
            emit errorOccurred(tr("Cannot add video output to capture session"));
            return false;
        }
        [session addOutput:output];

        [session startRunning];

        d->session  = session;
        d->output   = output;
        d->delegate = delegate;
    }

    m_state = State::Active;
    emit stateChanged(State::Active);
    return true;
}

void VideoInputApple::stop()
{
    if (!d->session)
        return;

    // Nil owner first so in-flight callbacks become no-ops.
    ((VideoInputAppleDelegate *)d->delegate).owner = nullptr;

    [d->session stopRunning];
    [d->output setSampleBufferDelegate:nil queue:nil];

    d->session  = nil;
    d->output   = nil;
    d->delegate = nil;

    if (m_state != State::Stopped) {
        m_state = State::Stopped;
        emit stateChanged(State::Stopped);
    }
}

void VideoInputApple::suspend()
{
    if (d->session && m_state == State::Active) {
        [d->session stopRunning];
        m_state = State::Suspended;
        emit stateChanged(State::Suspended);
    }
}

void VideoInputApple::resume()
{
    if (d->session && m_state == State::Suspended) {
        [d->session startRunning];
        m_state = State::Active;
        emit stateChanged(State::Active);
    }
}

bool VideoInputApple::isActive() const
{
    return m_state == State::Active;
}

QVideoFrameFormat VideoInputApple::frameFormat() const
{
    return QVideoFrameFormat{};
}

void VideoInputApple::onFrameCaptured(const QVideoFrame &frame)
{
    emit videoFrameReady(frame);
}
