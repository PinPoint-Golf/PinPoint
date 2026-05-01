#import <Speech/Speech.h>
#import <AVFAudio/AVFAudio.h>
#include "STTBackendApple.h"

#include <QDebug>
#include <QMetaObject>
#include <atomic>
#include <memory>

// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------

struct STTBackendApple::Private {
    SFSpeechRecognizer*       recognizer  = nil;
    SFSpeechRecognitionTask*  currentTask = nil;
    bool                      ready       = false;

    // Shared alive flag so blocks that outlive this object do nothing.
    // Set to false in the destructor before any member is freed.
    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);
};

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

STTBackendApple::STTBackendApple(QObject* parent)
    : STTBackend(parent)
    , d(new Private)
{
    d->recognizer = [[SFSpeechRecognizer alloc]
        initWithLocale:[NSLocale localeWithLocaleIdentifier:@"en-US"]];
    if (!d->recognizer)
        qWarning() << "[STTBackendApple] SFSpeechRecognizer unavailable for en-US locale";
}

STTBackendApple::~STTBackendApple()
{
    // Signal any in-flight result blocks not to touch this object.
    *d->alive = false;

    // Cancel any active recognition task; its result handler will receive
    // NSUserCancelledError and do nothing (handled below).
    if (d->currentTask) {
        [d->currentTask cancel];
        d->currentTask = nil;
    }
    delete d;
}

// ---------------------------------------------------------------------------
// STTBackend interface
// ---------------------------------------------------------------------------

bool STTBackendApple::loadModel(const QString& modelPath)
{
    Q_UNUSED(modelPath)
    if (!d->recognizer)
        return false;

    // Fast path: permission already determined (expected on normal startup after
    // requestSpeechRecognitionPermission() has been called from the main thread).
    SFSpeechRecognizerAuthorizationStatus status = [SFSpeechRecognizer authorizationStatus];

    if (status == SFSpeechRecognizerAuthorizationStatusAuthorized) {
        d->ready = [d->recognizer isAvailable];
        if (!d->ready)
            qWarning() << "[STTBackendApple] SFSpeechRecognizer is not currently available";
        return d->ready;
    }

    if (status != SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        qWarning() << "[STTBackendApple] Speech recognition not authorized (status"
                   << static_cast<int>(status)
                   << "). Grant access in System Settings → Privacy & Security → Speech Recognition.";
        return false;
    }

    // Fallback: status still undetermined — request it here via semaphore.
    // This should only happen if requestSpeechRecognitionPermission() was not
    // called before the processor thread started.
    __block SFSpeechRecognizerAuthorizationStatus authStatus = status;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SFSpeechRecognizer requestAuthorization:
        ^(SFSpeechRecognizerAuthorizationStatus s) {
            authStatus = s;
            dispatch_semaphore_signal(sem);
        }];

    // Wait up to 30 s for the user to respond to the dialog.
    const long timedOut = dispatch_semaphore_wait(
        sem, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));

    if (timedOut) {
        qWarning() << "[STTBackendApple] Timed out waiting for speech recognition authorization";
        return false;
    }
    if (authStatus != SFSpeechRecognizerAuthorizationStatusAuthorized) {
        qWarning() << "[STTBackendApple] Speech recognition authorization denied";
        return false;
    }

    d->ready = [d->recognizer isAvailable];
    if (!d->ready)
        qWarning() << "[STTBackendApple] SFSpeechRecognizer is not currently available";
    return d->ready;
}

void STTBackendApple::transcribe(const std::vector<float>& pcmF32)
{
    if (!d->ready || !d->recognizer) {
        emit transcriptionFailed(QStringLiteral("Apple STT not ready"));
        return;
    }
    if (pcmF32.empty()) {
        emit transcriptionFailed(QStringLiteral("Empty audio buffer"));
        return;
    }

    // Build an AVAudioPCMBuffer from the 16 kHz mono float32 data that
    // AudioConverter has already prepared.
    AVAudioFormat* fmt = [[AVAudioFormat alloc]
        initWithCommonFormat:AVAudioPCMFormatFloat32
                  sampleRate:16000.0
                    channels:1
                 interleaved:NO];

    const AVAudioFrameCount frameCount =
        static_cast<AVAudioFrameCount>(pcmF32.size());
    AVAudioPCMBuffer* pcmBuf = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:fmt
           frameCapacity:frameCount];
    pcmBuf.frameLength = frameCount;

    std::copy(pcmF32.begin(), pcmF32.end(), pcmBuf.floatChannelData[0]);

    // Configure the recognition request.
    SFSpeechAudioBufferRecognitionRequest* req =
        [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    req.shouldReportPartialResults = NO;

    // Prefer on-device recognition where available (macOS 13+, no network needed).
    if (@available(macOS 13.0, *)) {
        if ([d->recognizer supportsOnDeviceRecognition])
            req.requiresOnDeviceRecognition = YES;
    }

    [req appendAudioPCMBuffer:pcmBuf];
    [req endAudio];

    // Capture state needed by the async result block.
    auto  aliveFlag = d->alive;     // shared_ptr — outlives this object
    auto* self      = this;         // raw pointer — guarded by aliveFlag

    d->currentTask =
        [d->recognizer recognitionTaskWithRequest:req
            resultHandler:^(SFSpeechRecognitionResult* result, NSError* error) {
                if (!*aliveFlag)
                    return;

                if (error) {
                    // NSUserCancelledError == intentional cancellation; ignore.
                    if (error.code == NSUserCancelledError)
                        return;

                    const QString msg =
                        QString::fromNSString(error.localizedDescription);
                    // Post to the worker thread that owns self.
                    QMetaObject::invokeMethod(
                        self,
                        [self, msg, aliveFlag]() {
                            if (*aliveFlag)
                                emit self->transcriptionFailed(msg);
                        },
                        Qt::QueuedConnection);
                    return;
                }

                if (result && result.isFinal) {
                    const QString text =
                        QString::fromNSString(
                            result.bestTranscription.formattedString
                        ).trimmed();
                    if (!text.isEmpty()) {
                        QMetaObject::invokeMethod(
                            self,
                            [self, text, aliveFlag]() {
                                if (*aliveFlag)
                                    emit self->transcriptionReady(text);
                            },
                            Qt::QueuedConnection);
                    }
                }
            }];
}

bool STTBackendApple::isReady() const
{
    return d && d->ready;
}
