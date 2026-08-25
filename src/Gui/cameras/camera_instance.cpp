/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "camera_instance.h"

#include "app_settings.h"
#include "event_buffer.h"
#include "source_descriptor.h"
#include <cstring>
#include <utility>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include "ting_player.h"
#include "video_input_base.h"
#include "video_input_factory.h"
#include "video_preprocessor_base.h"
#include "bayer_video_item.h"
#include "frame_crop.h"
#ifdef HAVE_PPCP_TRANSPORT
#include "VideoInputPpcp.h"
#include "../../Ppcp/ppcp_host_peer.h"
#include "../../Ppcp/ppcp_host_service.h"
#endif
#ifdef HAVE_OPENCV
#include "video_preprocessor_opencv.h"
#include "frame_throttle.h"
#endif

#if defined(HAVE_OPENCV) && defined(HAVE_MOVENET) && defined(HAVE_ONNXRUNTIME)
#include "pose_estimator_movenet.h"
#endif

#ifdef HAVE_OPENCV
#include "ball_detector.h"
#endif

#include <QCoreApplication>
#include <QPointer>
#include <QMetaObject>
#include "pp_debug.h"
#include <algorithm>
#include <cmath>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include "pp_os_metrics.h"

#ifdef Q_OS_MACOS
#include "macos_permissions.h"
#endif

// Maps the backend-neutral PixelEncoding (from CameraCapabilities) to the
// pinpoint buffer type.  Used by both the constructor (pre-start estimate from
// enumerated capabilities) and updateBufferDescriptor() (post-start correction).
static pinpoint::PixelFormat bufferPixelFormat(PixelEncoding enc)
{
    switch (enc) {
    case PixelEncoding::YUV422_YUYV: return pinpoint::PixelFormat::YUYV;
    case PixelEncoding::YUV422_UYVY: return pinpoint::PixelFormat::UYVY;
    case PixelEncoding::YUV420_NV12: return pinpoint::PixelFormat::NV12;
    case PixelEncoding::YUV420_I420: return pinpoint::PixelFormat::YUV420P;
    case PixelEncoding::BGR8:        return pinpoint::PixelFormat::BGRA32;
    case PixelEncoding::BayerRG8:    return pinpoint::PixelFormat::BayerRG8;
    case PixelEncoding::BayerRG16:   return pinpoint::PixelFormat::BayerRG16;
    case PixelEncoding::BayerBG8:    return pinpoint::PixelFormat::BayerBG8;
    case PixelEncoding::BayerGR8:    return pinpoint::PixelFormat::BayerGR8;
    case PixelEncoding::BayerGB8:    return pinpoint::PixelFormat::BayerGB8;
    case PixelEncoding::BayerGB16:   return pinpoint::PixelFormat::BayerGB16;
    default:                          return pinpoint::PixelFormat::Unknown;
    }
}

// ---------------------------------------------------------------------------
// Constructors / destructor
// ---------------------------------------------------------------------------

CameraInstance::CameraInstance(QObject *parent)
    : QObject(parent)
    , m_captureThread(new QThread(this))
    , m_videoInput(VideoInputFactory::create(VideoInputFactory::Backend::Auto))
{
    setupPipeline();
}

CameraInstance::CameraInstance(const Device &device, pinpoint::EventBuffer *buffer,
                                 AppSettings *appSettings, QObject *parent)
    : QObject(parent)
    , m_captureThread(new QThread(this))
    , m_deviceId(device.id)
    , m_deviceDescription(device.description)
    , m_deviceSerialNumber(device.capabilities.serialNumber)
    , m_videoInput(VideoInputFactory::create(device.backend))
    , m_eventBuffer(buffer)
{
    // Read the saved alias from the shared AppSettings instance owned by main().
    // When no pointer is supplied (tests/tools) the alias is simply left empty.
    if (appSettings) {
        const QString key = device.description + QStringLiteral("|") +
            (device.capabilities.serialNumber.isEmpty() ? device.id
                                                         : device.capabilities.serialNumber);
        m_deviceAlias = appSettings->cameraAlias().value(key).toString();

        // Persisted crop must be known BEFORE the EventBuffer registration
        // below so the ring slots are sized for the cropped frame. Loaded for
        // preview instances too (the crop editor reads cropRoi), but only
        // buffer-backed instances ever apply it (m_cropEnabled below).
        const QVariantMap roiMap = appSettings->cameraRoi();
        if (roiMap.contains(key)) {
            const QVariantMap r = roiMap.value(key).toMap();
            const QRectF roi(r.value(QStringLiteral("x")).toDouble(),
                             r.value(QStringLiteral("y")).toDouble(),
                             r.value(QStringLiteral("w")).toDouble(),
                             r.value(QStringLiteral("h")).toDouble());
            if (pp_crop::cropIsActive(roi))
                m_cropRoi = roi.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        }
    }

    // Register the source before EventBuffer::start() is called in main().
    // Capabilities were queried at enumeration time (VideoInputFactory::enumerateDevices)
    // and stored in the Device struct — use them directly without re-opening the camera.
    if (m_eventBuffer) {
        const CameraCapabilities &caps = device.capabilities;

        pinpoint::SourceDescriptor desc;
        desc.name       = device.description.toStdString();
        // Stable per-device key (serial when present, else device id) so the
        // EventBuffer can keep session-lifetime stats keyed by identifier even
        // for UVC webcams that report no serial number. Mirrors ImuInstance.
        desc.identifier = (device.capabilities.serialNumber.isEmpty()
                           ? device.id
                           : device.capabilities.serialNumber).toStdString();

        pinpoint::CameraFormat cfmt{};

        // --- Pixel format ---
        // queryCapabilities() now populates defaultFormat from Qt's preferred
        // (first) video format pre-start, so we have a real pixel format here
        // without needing the camera to be running.  updateBufferDescriptor()
        // will call updateSourceFormat() to correct it to the actual negotiated
        // format once the camera is active.
        cfmt.pixel_format = bufferPixelFormat(caps.pixelFormat.defaultFormat.encoding);

        // --- Resolution ---
        // Slot capacity must fit any frame Qt might negotiate, so size for the
        // LARGEST supported resolution (not the default).  The actual negotiated
        // resolution is written into the descriptor by updateSourceFormat() once
        // the camera is active.
        int w = 0, h = 0;
        for (const Resolution &r : caps.resolution.presets) {
            if (r.width * r.height > w * h) { w = r.width; h = r.height; }
        }
        // GenICam cameras report a Range, not presets, and their
        // defaultResolution is the CURRENT region (possibly a stale crop from
        // a previous run) — use the range maximum, i.e. the full sensor.
        if (w == 0 && caps.resolution.kind == CapabilityKind::Range) {
            w = caps.resolution.widthRange.max;
            h = caps.resolution.heightRange.max;
        }
        if (w == 0) { w = caps.resolution.defaultResolution.width;
                      h = caps.resolution.defaultResolution.height; }
        if (w <= 0) { w = 1920; h = 1080; }
        m_sensorWidth  = w;
        m_sensorHeight = h;

        // Crop-aware slot dims: rounded UP to 16 so both the hardware ROI
        // (which snaps DOWN to device increments) and the software crop
        // (snapped down to even) always fit the slot.
        int slotW = w, slotH = h;
        if (pp_crop::cropIsActive(m_cropRoi)) {
            auto ceil16 = [](int v) { return (v + 15) & ~15; };
            slotW = std::clamp(ceil16(int(std::ceil(w * m_cropRoi.width()))),  16, w);
            slotH = std::clamp(ceil16(int(std::ceil(h * m_cropRoi.height()))), 16, h);
            m_expectedCropWidth  = slotW;
            m_expectedCropHeight = slotH;
            m_activeCropRoi      = m_cropRoi;
            m_cropEnabled        = true;
        }
        cfmt.width  = static_cast<uint32_t>(slotW);
        cfmt.height = static_cast<uint32_t>(slotH);

        // Seed the displayed frame size so tiles have the right aspect from
        // the moment the camera is selected, before any frame arrives. Use
        // the exact expected crop rect (not the ceil16 slot dims); delivered
        // frames remain the authority and override on any mismatch. No emit:
        // no bindings exist during construction.
        if (m_cropEnabled) {
            const QRect seed = pp_crop::snapCropRect(m_activeCropRoi,
                                                     m_sensorWidth, m_sensorHeight);
            if (!seed.isEmpty()) {
                m_frameWidth  = seed.width();
                m_frameHeight = seed.height();
            }
        } else {
            m_frameWidth  = m_sensorWidth;
            m_frameHeight = m_sensorHeight;
        }

        // --- Slot payload: worst-case bytes per pixel across all supported formats ---
        int maxBpp = 2; // NV12/YUYV baseline
        for (const PixelFormat &pf : caps.pixelFormat.supported) {
            int bpp = pf.bitsPerPixel > 0 ? (pf.bitsPerPixel + 7) / 8 : 0;
            if (bpp == 0) {
                switch (pf.encoding) {
                case PixelEncoding::RGBA8:
                case PixelEncoding::BGR8:          bpp = 4; break;
                case PixelEncoding::BayerRG16:
                case PixelEncoding::BayerGB16:
                case PixelEncoding::YUV422_YUYV:
                case PixelEncoding::YUV422_UYVY:   bpp = 2; break;
                default:                           bpp = 2; break;
                }
            }
            maxBpp = std::max(maxBpp, bpp);
        }
        // Ring slot capacity must be predicted before any frame exists. The
        // AVFoundation backend converts every frame to 32-bpp ARGB32, which the
        // enumerated (NV12/YUYV) capabilities under-report — floor the worst case at
        // 4 bytes/pixel so a delivered frame always fits. This is a capacity bound
        // only; the actual pixel format and resolution are stamped from the first
        // real frame in updateBufferDescriptor(), not guessed here.
        // (Regression guard for 90d92a0, which moved slot sizing off the live
        // VideoInputApple::queryCapabilities() onto the enumerated Qt capabilities.)
        if (device.backend == VideoInputFactory::Backend::AppleAVFoundation)
            maxBpp = std::max(maxBpp, 4);
        // Allow up to 128 bytes of per-row alignment padding: backends can
        // allocate frames with padded bytesPerLine, and publishFrameToBuffer
        // stores mappedBytes(p) (padding included). Without the headroom a
        // padded frame at max resolution exceeds the slot and every frame is
        // dropped (loudly, post the pre-acquire size check — but still dropped).
        cfmt.max_payload_bytes     = (cfmt.width * static_cast<uint32_t>(maxBpp) + 128u)
                                     * cfmt.height;
        cfmt.typical_payload_bytes = cfmt.max_payload_bytes;

        // --- FPS: use the maximum supported rate for worst-case slot count ---
        double fps = caps.frameRate.range.max > 0.0 ? caps.frameRate.range.max : 60.0;
        cfmt.fps_numerator   = static_cast<uint32_t>(fps * 1000.0);
        cfmt.fps_denominator = 1000;

        switch (device.backend) {
        case VideoInputFactory::Backend::AppleAVFoundation:
            desc.format.device = pinpoint::DeviceKind::Camera_AVFoundation;
            break;
        case VideoInputFactory::Backend::Aravis:
        case VideoInputFactory::Backend::Spinnaker:
            desc.format.device     = pinpoint::DeviceKind::Camera_GenICam;
            // GenICam cameras (Spinnaker/Aravis) can run well above 60 fps.
            // Use 200 fps as the conservative upper bound so the ring is sized
            // for ceil(200*5)=1000 → 1024 slots, enough for 150 fps cameras.
            // At 60 fps the ring was 512 slots, causing constant overwrites at
            // 150 fps (150*5=750 frames > 512 ring capacity).
            cfmt.fps_numerator   = 200;
            cfmt.fps_denominator = 1;
            break;
        default:
            desc.format.device = pinpoint::DeviceKind::Camera_UVC;
            break;
        }
        desc.format.format            = cfmt;
        desc.window_duration          = std::chrono::milliseconds(5000);
        desc.expected_interarrival_us =
            std::chrono::microseconds(static_cast<int64_t>(1'000'000.0 / fps));
        desc.sync_source              = pinpoint::SyncSource::SoftwareTimestamp;

        ppInfo() << "[CameraInstance] registering buffer source:"
                 << device.description
                 << (device.capabilities.serialNumber.isEmpty()
                     ? QString() : "(" + device.capabilities.serialNumber + ")")
                 << cfmt.width << "x" << cfmt.height
                 << "@ max" << fps << "fps"
                 << "slot_bytes:" << desc.computeSlotBytes()
                 << "slots:" << desc.computeSlotCount();

        // CameraManager::setSelected() ensures the buffer is Paused before
        // creating this controller, so registration is always safe.
        // Memory is held until deregisterFromBuffer() is called on deselection.
        m_sourceId = m_eventBuffer->registerSource(desc);
    }

#ifdef HAVE_PPCP
    // Read before setupPipeline() — it decides there whether m_videoInput
    // moves to m_captureThread at all.
    m_isPpcpBackend = (device.backend == VideoInputFactory::Backend::Ppcp);
#endif

    setupPipeline();
}

void CameraInstance::deregisterFromBuffer()
{
    if (!m_eventBuffer || m_sourceId == pinpoint::kInvalidSourceId) return;
    m_eventBuffer->deregisterSource(m_sourceId);
    m_sourceId = pinpoint::kInvalidSourceId;
}

void CameraInstance::setupPipeline()
{
    // Set to false to disable the entire preprocessor/pose/throttle pipeline
    // for capture-rate diagnostics.  Flip back to true to restore.
    static constexpr bool kPoseEnabled = true;

    m_tingPlayer = new TingPlayer(this);

    m_captureThread->setObjectName(QStringLiteral("VideoCaptureThread"));
    // A PPCP-backed instance stays on THIS (the GUI) thread — see
    // m_isPpcpBackend's header comment.  It owns no socket of its own and is
    // fed entirely by PpcpHostService's already-running GUI-thread pump
    // (VideoInputPpcp::dispatchEvent()/applyTimebaseOffsets()); moving it to
    // m_captureThread would make those writes cross-thread with no
    // synchronization.  m_captureThread itself is still constructed (cheap)
    // but never started for this backend.
    if (!m_isPpcpBackend) m_videoInput->moveToThread(m_captureThread);

#ifdef HAVE_OPENCV
    if (kPoseEnabled && m_eventBuffer) {
        m_preprocessThread = new QThread(this);
        m_preprocessThread->setObjectName(QStringLiteral("VideoPreprocessThread"));
        m_preprocessor = new VideoPreprocessorOpenCV();
        m_preprocessor->moveToThread(m_preprocessThread);
        connect(m_preprocessor, &VideoPreprocessorBase::preprocessStatsUpdated,
                this, &CameraInstance::onPreprocessStats, Qt::QueuedConnection);
        m_preprocessThread->start();

        auto *cvPP = qobject_cast<VideoPreprocessorOpenCV *>(m_preprocessor);

#if defined(HAVE_MOVENET) && defined(HAVE_ONNXRUNTIME)
        m_poseThread = new QThread(this);
        m_poseThread->setObjectName(QStringLiteral("PoseEstimatorThread"));
        auto *mn = new PoseEstimatorMoveNet();
        m_poseEstimator = mn;
        m_poseEstimator->moveToThread(m_poseThread);
        connect(m_poseThread, &QThread::started, mn, &PoseEstimatorMoveNet::load);
        // Symmetric unregister (see Camera.Capture above): context-free so it runs
        // on the pose thread as it finishes, matching the tid registered in load().
        // Without it the dead thread's pthread_t dangles and the profiler crashes.
        connect(m_poseThread, &QThread::finished, []() {
            pinpoint::osmetrics::unregisterThread();
        });
        connect(m_poseEstimator, &PoseEstimatorBase::poseStatsUpdated,
                this, &CameraInstance::onPoseStats, Qt::QueuedConnection);
        connect(m_poseEstimator, &PoseEstimatorBase::poseBackendReady,
                this, &CameraInstance::onPoseBackendReady, Qt::QueuedConnection);

        m_frameThrottle = new FrameThrottle();
        m_frameThrottle->setSkipFactor(2);
        m_frameThrottle->setConsumerCount(2);     // pose estimator + ball detector
        m_frameThrottle->setRawConsumerCount(2);  // same two consumers on the raw path

        connect(cvPP, &VideoPreprocessorOpenCV::framePreprocessed,
                m_poseEstimator, &PoseEstimatorBase::estimatePose, Qt::QueuedConnection);
        connect(m_poseEstimator, &PoseEstimatorBase::estimationDone,
                m_frameThrottle, &FrameThrottle::clearBusy, Qt::DirectConnection);
        connect(m_poseEstimator, &PoseEstimatorBase::estimationDone,
                m_frameThrottle, &FrameThrottle::clearRawBusy, Qt::DirectConnection);
        connect(m_poseEstimator, &PoseEstimatorBase::poseEstimated,
                this, &CameraInstance::onPoseEstimated, Qt::QueuedConnection);
        m_poseThread->start();
#endif // HAVE_MOVENET && HAVE_ONNXRUNTIME

        // Ball detector — connects to the same preprocessed frame signal,
        // runs on its own thread so it never stalls the pose estimator.
        m_ballThread = new QThread(this);
        m_ballThread->setObjectName(QStringLiteral("BallDetectorThread"));
        m_ballDetector = new BallDetector();
        m_ballDetector->moveToThread(m_ballThread);

        connect(cvPP, &VideoPreprocessorOpenCV::framePreprocessed,
                m_ballDetector, &BallDetector::detect, Qt::QueuedConnection);
        connect(m_ballDetector, &BallDetector::ballDetected,
                this, &CameraInstance::onBallDetected, Qt::QueuedConnection);
        connect(m_ballDetector, &BallDetector::baselineReady,
                this, &CameraInstance::onBallBaselineReady, Qt::QueuedConnection);
        // Struck-ball launch → shot-arbiter candidate. The detector reports the
        // triggering frame's capture time (buffer clock) and the impact age before
        // it, so true impact = launchFrameTUs − launchAgeUs; fall back to now only
        // for a defensive frame time of -1. conf 0.6 sits below the arbiter's 0.8
        // lone-candidate self-commit floor, so a spurious vision launch can only
        // corroborate, never commit alone.
        connect(m_ballDetector, &BallDetector::ballLaunched, this,
                [this](qint64 launchFrameTUs, qint64 launchAgeUs, float x, float y) {
            const qint64 baseTUs = launchFrameTUs >= 0 ? launchFrameTUs
                                                       : pinpoint::EventBuffer::nowMicros();
            const qint64 estImpactUs = baseTUs - launchAgeUs;
            // First launch of a striking sequence wins: the struck ball can
            // bounce back through the ROI, re-lock, and fire a second launch
            // within the postroll (observed 0.53 s after impact, 2026-07-10
            // sw_0005 — it overwrote the real launch before the window-freeze
            // snapshot). Two genuine shots are never < 2 s apart (arbiter
            // refractory 1.5 s + the processing pipeline), so a launch that
            // close to the previous one is the same strike — keep the first.
            const bool relaunch = m_ballLaunchTUs >= 0
                               && estImpactUs - m_ballLaunchTUs < 2'000'000;
            if (!relaunch) {
                // v3.4 (design §9.3): stash the launch position the detector
                // already computed — previously discarded here.
                // CameraManager::ballLaunched and the shot-arbiter path only
                // ever needed the timestamp, so that signal/signature is
                // unchanged; the position feeds the offline ball-anchor pass
                // via ballLaunchInfo().
                m_ballLaunchTUs = estImpactUs;
                m_ballLaunchX   = double(x);
                m_ballLaunchY   = double(y);
                emit ballLaunched(estImpactUs, 0.6f);
            }
        }, Qt::QueuedConnection);
        // Wire BOTH completion signals to BOTH throttle paths (mirrors the
        // pose estimator above): a camera feeds exactly one path, so only
        // that path's counter is consulted — but without the raw wiring the
        // raw (Bayer) path ran the ball detector completely unthrottled,
        // queueing unbounded full-frame Mats whenever detection was slower
        // than the pose cycle.
        connect(m_ballDetector, &BallDetector::ballDetected,
                m_frameThrottle, &FrameThrottle::clearBusy, Qt::DirectConnection);
        connect(m_ballDetector, &BallDetector::detectionSkipped,
                m_frameThrottle, &FrameThrottle::clearBusy, Qt::DirectConnection);
        connect(m_ballDetector, &BallDetector::ballDetected,
                m_frameThrottle, &FrameThrottle::clearRawBusy, Qt::DirectConnection);
        connect(m_ballDetector, &BallDetector::detectionSkipped,
                m_frameThrottle, &FrameThrottle::clearRawBusy, Qt::DirectConnection);

        // Forward ROI changes to the detector thread.  The lambda captures the
        // current roi value on the main thread before posting it to the detector.
        connect(this, &CameraInstance::roiChanged, this, [this]() {
            QMetaObject::invokeMethod(m_ballDetector, "setRoi",
                Qt::QueuedConnection, Q_ARG(QRectF, m_roi));
        }, Qt::DirectConnection);

        m_ballThread->start();

    } // kPoseEnabled
#endif // HAVE_OPENCV

    connectVideoInput();

    // On first Active state: register the buffer source with actual format/fps,
    // and re-evaluate needsDebayer (only meaningful after Active).
    connect(m_videoInput, &VideoInputBase::stateChanged,
            this, [this](VideoInputBase::State s) {
                if (s == VideoInputBase::State::Active) {
                    setConnecting(false);   // device live — connect window over
                    if (!m_lastPreviewError.isEmpty()) {
                        m_lastPreviewError.clear();
                        emit lastPreviewErrorChanged();
                    }
                    updateBufferDescriptor();
                    emit needsDebayerChanged();

                    // queryCapabilities() must run on the capture thread where
                    // m_videoInput (and its camera handle) lives.  Post it there,
                    // then deliver the result back to the main thread.
                    // By the time this queued call runs, frames are flowing and
                    // m_sink->videoFrame() is valid — so queryCapabilities() returns
                    // the true negotiated format.  Re-run updateBufferDescriptor()
                    // here to correct any mismatch from the immediate call above.
                    QMetaObject::invokeMethod(m_videoInput, [this]() {
                        CameraCapabilities caps = m_videoInput->queryCapabilities();
                        QMetaObject::invokeMethod(this, [this, caps = std::move(caps)]() {
                            int w     = caps.resolution.defaultResolution.width;
                            int h     = caps.resolution.defaultResolution.height;
                            double fps = caps.frameRate.range.defaultValue;
                            // Capability dims are the camera's DELIVERY size —
                            // pre-crop for software-cropped backends (the
                            // backend's internal sink sees raw camera frames).
                            // Only seed when no frame has arrived yet, crop-
                            // adjusted so the tile aspect is right pre-first-
                            // frame; delivered frames in drainDisplayFrame /
                            // drainRawFrame are the authority and override.
                            if (w > 0 && h > 0 && m_frameWidth == 0) {
                                if (m_cropEnabled) {
                                    const QRect r = pp_crop::snapCropRect(m_activeCropRoi, w, h);
                                    if (!r.isEmpty()) { w = r.width(); h = r.height(); }
                                }
                                m_frameWidth  = w;
                                m_frameHeight = h;
                                emit frameSizeChanged();
                            }
                            if (fps > 0)
                                m_configuredFps = fps;
                            updateBufferDescriptor();
                        });
                    }, Qt::QueuedConnection);
                }
            }, Qt::QueuedConnection);

    // Register the capture thread with the resource profiler (runs on it: the
    // receiver m_videoInput lives on m_captureThread, so the slot fires there).
    connect(m_captureThread, &QThread::started, m_videoInput, []() {
        pinpoint::osmetrics::registerThread("Camera.Capture");
    });
    // Symmetric unregister, or the profiler crashes when this camera is torn down:
    // a dead thread's entry would linger alive=true and the next sampler tick would
    // call pthread_getcpuclockid() on its now-dangling pthread_t (SIGSEGV). Must be
    // a context-free 3-arg connect so the functor runs on the capture thread itself
    // as it finishes (currentTid() must match the registered entry). A receiver here
    // would be wrong: stopCapture() moves m_videoInput back to the GUI thread before
    // quit(), so an AutoConnection would fire on the GUI thread and unregister it.
    connect(m_captureThread, &QThread::finished, []() {
        pinpoint::osmetrics::unregisterThread();
    });

    // Never started for a PPCP-backed instance (see the moveToThread guard
    // above) — the registerThread/unregisterThread pair above simply never
    // fires for it, which is correct: there is no dedicated capture thread to
    // profile.
    if (!m_isPpcpBackend) m_captureThread->start();

    // Sample the capture-thread frame counter every 500 ms to compute actual fps.
    m_fpsSampleTimer = new QTimer(this);
    m_fpsSampleTimer->setInterval(500);
    connect(m_fpsSampleTimer, &QTimer::timeout, this, [this]() {
        const int count   = m_frameCaptureCount.exchange(0, std::memory_order_relaxed);
        const double secs = m_fpsSampleElapsed.restart() / 1000.0;
        if (secs > 0.0) {
            const double fps = count / secs;
            if (!qFuzzyCompare(m_cameraFps, fps)) {
                m_cameraFps = fps;
                emit cameraFpsChanged();
            }
        }
    });
    m_fpsSampleElapsed.start();
    m_fpsSampleTimer->start();
}

void CameraInstance::stopCapture()
{
    if (m_isPpcpBackend) {
        // Already on this thread — no invokeMethod/moveToThread needed, and
        // m_captureThread was never started so isRunning() below would
        // otherwise skip stop() entirely, leaving the preview Stream open.
        if (m_videoInput) m_videoInput->stop();
#ifdef HAVE_PPCP_TRANSPORT
        if (auto *v = dynamic_cast<VideoInputPpcp *>(m_videoInput)) v->detach();
#endif
        return;
    }
    if (m_captureThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoInput, [this]() {
            m_videoInput->stop();
            m_videoInput->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);
        m_captureThread->quit();
        m_captureThread->wait();
    }
}

#ifdef HAVE_PPCP_TRANSPORT
void CameraInstance::ppcpAttachIfNeeded()
{
    if (!m_isPpcpBackend || !m_ppcpHostService) return;
    QString peerId, sourceId;
    if (!VideoInputPpcp::parseDeviceId(m_deviceId, &peerId, &sourceId)) return;
    Ppcp::PpcpHostPeer *hp = m_ppcpHostService->peerForId(peerId);
    // Left unattached rather than guessed at — start()'s own "no peer
    // attached" / "no session open" refusals are the honest report of
    // exactly this: the phone isn't connected right now, or declared but the
    // Session it needs failed to open (both logged where they happen, not
    // here — this function has nothing more specific to say).
    if (!hp || !hp->liveSession().isOpen()) return;
    if (auto *v = dynamic_cast<VideoInputPpcp *>(m_videoInput))
        v->attach(hp->liveSession().peer(),
                 QString::fromStdString(hp->liveSession().config().sessionId));
}
#endif  // HAVE_PPCP_TRANSPORT

CameraInstance::~CameraInstance()
{
    // Shutdown in pipeline order so no stage receives frames after it is torn down.

    // 1. Stop capture — no more videoFrameReady signals after this.
    stopCapture();
    delete m_videoInput;
    m_videoInput = nullptr;

    // 2. Stop preprocess — no more framePreprocessed signals after this.
    if (m_preprocessor && m_preprocessThread && m_preprocessThread->isRunning()) {
        QMetaObject::invokeMethod(m_preprocessor, [this]() {
            m_preprocessor->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);
        m_preprocessThread->quit();
        m_preprocessThread->wait();
    }
    delete m_preprocessor;
    m_preprocessor = nullptr;
    // m_frameThrottle is deleted AFTER the pose/ball threads are joined below:
    // their completion signals (estimationDone, ballDetected/detectionSkipped)
    // are DirectConnection into clearBusy/clearRawBusy, so an inference still
    // draining on either thread would call into a freed throttle.

#ifdef HAVE_OPENCV
    // 3. Stop pose estimator — drain queued estimatePose calls first.
    if (m_poseEstimator && m_poseThread && m_poseThread->isRunning()) {
        QMetaObject::invokeMethod(m_poseEstimator, [this]() {
            m_poseEstimator->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);
        m_poseThread->quit();
        m_poseThread->wait();
    }
    delete m_poseEstimator;
    m_poseEstimator = nullptr;

    // 4. Stop ball detector.
    if (m_ballDetector && m_ballThread && m_ballThread->isRunning()) {
        QMetaObject::invokeMethod(m_ballDetector, [this]() {
            m_ballDetector->moveToThread(QCoreApplication::instance()->thread());
        }, Qt::BlockingQueuedConnection);
        m_ballThread->quit();
        m_ballThread->wait();
    }
    delete m_ballDetector;
    m_ballDetector = nullptr;
#endif

    delete m_frameThrottle;
    m_frameThrottle = nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void CameraInstance::connectVideoInput()
{
    // Captured by value per backend instance: re-evaluated when the
    // auto-fallback path swaps in a QtMultimedia backend and reconnects.
    const bool hwCrop = m_videoInput->supportsHardwareCrop();

    // Standard QVideoFrame display path — used by Qt Multimedia and Aravis.
    // DirectConnection so only the freshest frame is ever queued to the main
    // thread. The software crop runs ONCE here, upstream of every consumer
    // (buffer, display, pose throttle), so all downstream sizing — descriptor
    // stamping, replay, tile aspect — follows the cropped frame automatically.
    VideoInputBase *const input = m_videoInput;   // as hwCrop: per backend instance
    connect(m_videoInput, &VideoInputBase::videoFrameReady,
            this, [this, hwCrop, input](const QVideoFrame &frame) {
                m_frameCaptureCount.fetch_add(1, std::memory_order_relaxed);

                // One capture-clock reading for this frame, sampled before any
                // crop/publish/queue work — shared by the ring entry AND the
                // pose/ball throttle so both land on the buffer clock at offset
                // zero (ball samples align with window frame timestamps).
                //
                // A backend that knows WHEN the frame was exposed says so, and
                // is believed (work package H4): arrival time is a good enough
                // proxy for a camera on this machine's bus and a bad one for a
                // frame that crossed a link. 0 means the backend has no instant
                // of its own, which is every local camera.
                qint64 frameTUs = input->lastFrameInstantUs();
                if (frameTUs == 0) frameTUs = pinpoint::EventBuffer::nowMicros();

                // Software crop: engages when a crop is configured, the
                // pipeline is live (not preview-only), and either the backend
                // has no hardware ROI or the frame arrived larger than the
                // expected hardware-cropped size (hardware ROI failed).
                // Hardware snapping rounds DOWN and the expected dims round
                // UP (ceil16), so a hardware-cropped frame never double-crops.
                QVideoFrame f = frame;
                if (m_cropEnabled && !m_previewOnly.load(std::memory_order_relaxed)
                        && (!hwCrop || frame.width()  > m_expectedCropWidth
                                    || frame.height() > m_expectedCropHeight)) {
                    f = pp_crop::cropVideoFrame(frame, m_activeCropRoi);
                }

                // EventBuffer publish (zero-copy into pre-allocated ring slot)
                if (m_eventBuffer && m_sourceId != pinpoint::kInvalidSourceId
                        && m_eventBuffer->isCapturing()) {
                    publishFrameToBuffer(f, frameTUs);
                }

                {
                    QMutexLocker lk(&m_latestFrameMutex);
                    m_latestDisplayFrame = f;
                }
                if (!m_displayFramePending.exchange(true, std::memory_order_acq_rel)) {
                    QMetaObject::invokeMethod(this, &CameraInstance::drainDisplayFrame,
                                             Qt::QueuedConnection);
                }

#ifdef HAVE_OPENCV
                // Pose/ball throttle path — suppressed in preview-only mode
                // so the inference pipeline never runs.
                if (m_frameThrottle && !m_previewOnly.load(std::memory_order_relaxed))
                    m_frameThrottle->offer(f, frameTUs);
#endif
            }, Qt::DirectConnection);

    // Raw Bayer display path — used by Spinnaker (and future Bayer backends).
    // Replaces the QVideoFrame path for cameras that emit rawVideoFrameReady.
    connect(m_videoInput, &VideoInputBase::rawVideoFrameReady,
            this, [this, hwCrop, input](const RawVideoFrame &frame) {
                m_frameCaptureCount.fetch_add(1, std::memory_order_relaxed);

                // One capture-clock reading for this frame — see the QVideoFrame
                // path above, including why a backend's own instant wins.
                qint64 frameTUs = input->lastFrameInstantUs();
                if (frameTUs == 0) frameTUs = pinpoint::EventBuffer::nowMicros();

                // Software crop fallback — same rule as the QVideoFrame path.
                RawVideoFrame f = frame;
                if (m_cropEnabled && !m_previewOnly.load(std::memory_order_relaxed)
                        && (!hwCrop || frame.width  > m_expectedCropWidth
                                    || frame.height > m_expectedCropHeight)) {
                    f = pp_crop::cropRawFrame(frame, m_activeCropRoi);
                }

                // EventBuffer publish for raw Bayer frames
                if (m_eventBuffer && m_sourceId != pinpoint::kInvalidSourceId
                        && m_eventBuffer->isCapturing()) {
                    publishRawFrameToBuffer(f, frameTUs);
                }

                {
                    QMutexLocker lk(&m_latestRawFrameMutex);
                    m_latestRawFrame = f;
                }
                if (!m_displayFramePending.exchange(true, std::memory_order_acq_rel)) {
                    QMetaObject::invokeMethod(this, &CameraInstance::drainRawFrame,
                                             Qt::QueuedConnection);
                }

#ifdef HAVE_OPENCV
                if (m_frameThrottle && !m_previewOnly.load(std::memory_order_relaxed))
                    m_frameThrottle->offerRaw(f, frameTUs);
#endif
            }, Qt::DirectConnection);

    connect(m_videoInput, &VideoInputBase::errorOccurred,
            this, &CameraInstance::onVideoError);

#ifdef HAVE_OPENCV
    if (m_frameThrottle) {
        // UniqueConnection: throttle and preprocessor survive the auto-
        // fallback backend swap, and connectVideoInput() runs again for the
        // replacement input — a duplicate here would run inference twice per
        // released frame and double-fire the throttle's done counters.
        connect(m_frameThrottle, &FrameThrottle::frameReady,
                m_preprocessor, &VideoPreprocessorBase::processFrame,
                static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
        connect(m_frameThrottle, &FrameThrottle::rawFrameReady,
                m_preprocessor, &VideoPreprocessorBase::processRawFrame,
                static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    }
#endif
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

bool   CameraInstance::isRecording()    const { return m_recording; }
bool   CameraInstance::isConnecting()   const { return m_connecting; }
bool   CameraInstance::isAravis()       const { return VideoInputFactory::backendType(m_videoInput) == VideoInputFactory::Backend::Aravis; }

void CameraInstance::setConnecting(bool on)
{
    if (m_connecting == on) return;
    m_connecting = on;
    emit isConnectingChanged();
}
bool   CameraInstance::isSpinnaker()    const { return VideoInputFactory::backendType(m_videoInput) == VideoInputFactory::Backend::Spinnaker; }
bool   CameraInstance::needsDebayer()   const { return isSpinnaker() && m_videoInput->emitsRawBayer(); }
double  CameraInstance::preprocessAvgMs()        const { return m_preprocessAvgMs; }
double  CameraInstance::cameraFps()              const { return m_cameraFps; }
int     CameraInstance::frameWidth()             const { return m_frameWidth; }
int     CameraInstance::frameHeight()            const { return m_frameHeight; }
double  CameraInstance::configuredFps()          const { return m_configuredFps; }
double  CameraInstance::poseAvgMs()              const { return m_poseAvgMs; }
double  CameraInstance::poseFps()                const { return m_poseFps; }
double  CameraInstance::ballAvgMs()              const { return m_ballAvgMs; }
QString CameraInstance::poseBackendLabel()       const { return m_poseBackendLabel; }
int     CameraInstance::moveNetModel()           const { return m_moveNetModel; }
QString CameraInstance::deviceDescription()      const { return m_deviceDescription; }
QString CameraInstance::deviceSerialNumber()     const { return m_deviceSerialNumber; }
QString CameraInstance::cameraKey()              const
{
    // Must mirror CameraManager::cameraKey(): "Model|Serial", falling back to
    // "Model|DeviceId" when the camera reports no serial.
    return m_deviceDescription + QStringLiteral("|")
         + (m_deviceSerialNumber.isEmpty() ? m_deviceId : m_deviceSerialNumber);
}
QString CameraInstance::deviceAlias()            const { return m_deviceAlias; }

void CameraInstance::setDeviceAlias(const QString &alias)
{
    if (m_deviceAlias == alias) return;
    m_deviceAlias = alias;
    emit deviceAliasChanged();
}

int     CameraInstance::perspective()            const { return m_perspective; }
bool    CameraInstance::isMirrored()             const { return m_isMirrored; }

void CameraInstance::setPerspective(int p)
{
    if (m_perspective == p)
        return;
    m_perspective = p;
    emit perspectiveChanged();
}

void CameraInstance::setIsMirrored(bool mirrored)
{
    if (m_isMirrored == mirrored)
        return;
    m_isMirrored = mirrored;
    emit isMirroredChanged();
}

bool    CameraInstance::moveNetThunderAvailable() const
{
#if defined(HAVE_OPENCV) && defined(HAVE_MOVENET) && defined(HAVE_ONNXRUNTIME)
    return PoseEstimatorMoveNet::isVariantAvailable(PoseEstimatorMoveNet::ModelVariant::Thunder);
#else
    return false;
#endif
}

QVariantList CameraInstance::poseKeypoints() const { return m_poseKeypoints; }

bool CameraInstance::poseEnabled() const { return m_poseEnabled; }

void CameraInstance::setPoseEnabled(bool on)
{
    if (m_poseEnabled == on)
        return;
    m_poseEnabled = on;
#ifdef HAVE_OPENCV
    if (m_poseEstimator)
        m_poseEstimator->setEnabled(on);   // atomic — safe to call cross-thread
#endif
    if (!on && !m_poseKeypoints.isEmpty()) {
        m_poseKeypoints.clear();
        emit poseKeypointsChanged();       // empty overlays immediately
    }
    emit poseEnabledChanged();
}

bool CameraInstance::ballEnabled() const { return m_ballEnabled; }

void CameraInstance::setBallEnabled(bool on)
{
    if (m_ballEnabled == on)
        return;
    m_ballEnabled = on;
#ifdef HAVE_OPENCV
    if (m_ballDetector)
        m_ballDetector->setEnabled(on);    // atomic — safe to call cross-thread
#endif
    emit ballEnabledChanged();
}

QRectF CameraInstance::roi() const { return m_roi; }

void CameraInstance::setRoi(QRectF roi)
{
    roi = roi.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (roi.isEmpty() || m_roi == roi)
        return;
    m_roi = roi;
    // The mat under the new box differs — invalidate the cache now, on the UI
    // thread, rather than waiting for the detector's async re-seed, so a shot
    // fired mid-reseed exports no baseline rather than one over the old ROI.
    m_ballBaseline = {};
    emit roiChanged();
    // The detector re-seeds its empty-mat baseline on the ROI change (queued to
    // the detector thread via the roiChanged → setRoi connection).
}

void CameraInstance::clearRoi()
{
    if (m_roi.isEmpty())
        return;
    m_roi = QRectF();
    emit roiChanged();
    if (m_ballDetected) {
        m_ballDetected = false;
        m_ballX = m_ballY = m_ballRadius = 0.0;
        emit ballDetectedChanged();
    }
    m_ballWindow.clear();
    m_ballPresentCount = 0;
    if (m_ballPresent) {
        m_ballPresent = false;
        emit ballPresentChanged(false);   // same contract as the recording-stop path
    }
    if (m_ballPresencePercent != 0.0) {
        m_ballPresencePercent = 0.0;
        emit ballPresencePercentChanged();
    }
}

void CameraInstance::relearnBallBaseline()
{
    // Invalidate the cache immediately — the reseed is async, so a shot fired
    // before it completes exports no baseline rather than the stale one being
    // replaced (pre-feature behavior for that window).
    m_ballBaseline = {};
#ifdef HAVE_OPENCV
    if (m_ballDetector)
        QMetaObject::invokeMethod(m_ballDetector, "relearnBaseline", Qt::QueuedConnection);
#endif
}

QRectF CameraInstance::cropRoi() const { return m_cropRoi; }

void CameraInstance::setCropRoi(QRectF roi)
{
    roi = roi.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (m_cropRoi == roi) return;
    m_cropRoi = roi;
    emit cropRoiChanged();
}

void CameraInstance::clearCropRoi()
{
    if (m_cropRoi.isEmpty()) return;
    m_cropRoi = QRectF();
    emit cropRoiChanged();
}

bool   CameraInstance::ballDetected()        const { return m_ballDetected; }
double CameraInstance::ballX()               const { return m_ballX; }
double CameraInstance::ballY()               const { return m_ballY; }
double CameraInstance::ballRadius()          const { return m_ballRadius; }
double CameraInstance::ballPresencePercent() const { return m_ballPresencePercent; }
bool   CameraInstance::ballPresent()         const { return m_ballPresent; }

#ifdef HAVE_OPENCV
void CameraInstance::onBallDetected(const BallDetection &result)
{
    if (m_replaying) return;

    // Drop results already in flight when the detector was disabled — a stale
    // found=false event here could fire a spurious ball-lost transition.
    if (!m_ballEnabled) return;

    // v3.4 (design §9.7): accumulate the per-frame ball stream — the low-entropy
    // "constant plus one step" record ShotProcessor later reads to build the
    // live ShotAnalysisJob::ballTrack / swing.json "ball" block. Stamp with the
    // frame's capture time (buffer clock, shared with the window frames); fall
    // back to now only for a stale pre-upgrade result carrying no frame time.
    const qint64 sampleTUs = result.tUs >= 0 ? result.tUs
                                             : pinpoint::EventBuffer::nowMicros();
    m_ballAccum.push_back({sampleTUs, result.found,
                           result.x, result.y, result.radius, result.score});
    trimBallAccum();

    // EMA of detection latency — only when detect() actually ran (detectMs > 0).
    if (result.detectMs > 0) {
        constexpr double kAlpha = 0.1;
        const double ms = m_ballAvgMs == 0.0
                        ? static_cast<double>(result.detectMs)
                        : kAlpha * static_cast<double>(result.detectMs) + (1.0 - kAlpha) * m_ballAvgMs;
        if (!qFuzzyCompare(m_ballAvgMs, ms)) {
            m_ballAvgMs = ms;
            emit ballAvgMsChanged();
        }
    }

    // Update the rolling 50-frame window.
    m_ballWindow.push_back(result.found);
    if (result.found)
        ++m_ballPresentCount;
    if (static_cast<int>(m_ballWindow.size()) > kBallWindowSize) {
        if (m_ballWindow.front())
            --m_ballPresentCount;
        m_ballWindow.pop_front();
    }
    const int pct = m_ballWindow.empty() ? 0
                  : static_cast<int>(std::round(m_ballPresentCount * 100.0 / static_cast<int>(m_ballWindow.size())));
    if (pct != static_cast<int>(std::round(m_ballPresencePercent))) {
        m_ballPresencePercent = static_cast<double>(pct);
        emit ballPresencePercentChanged();
    }

    // Fire ting and notify CameraManager when threshold-based presence flips.
    const bool nowPresent = (m_ballPresencePercent > kBallPresentThreshold);
    if (m_ballPresent != nowPresent) {
        if (!m_ballPresent && nowPresent)
            m_tingPlayer->play();
        m_ballPresent = nowPresent;
        emit ballPresentChanged(nowPresent);
    }

    // Only emit ballDetectedChanged when the present/absent state flips.
    if (!result.found && !m_ballDetected)
        return;

    m_ballDetected = result.found;
    m_ballX        = result.x;
    m_ballY        = result.y;
    m_ballRadius   = result.radius;
    emit ballDetectedChanged();
}

// See ballBaseline(): converts the detector's cv::Mat snapshot to the cv-free
// cache, replacing whatever was cached before (a stale/mid-reseed cache was
// already cleared synchronously by setRoi()/relearnBallBaseline()).
void CameraInstance::onBallBaselineReady(const BallBaselineSnapshot &snap)
{
    if (!snap.valid()) return;
    const cv::Mat &B = snap.B;
    const cv::Mat cont = B.isContinuous() ? B : B.clone();
    const size_t bytes = cont.total() * cont.elemSize();
    m_ballBaseline.blob   = QByteArray(reinterpret_cast<const char *>(cont.data),
                                       static_cast<qsizetype>(bytes));
    m_ballBaseline.w      = cont.cols;
    m_ballBaseline.h      = cont.rows;
    m_ballBaseline.roi    = snap.roi;
    m_ballBaseline.rHat   = snap.rHat;
    m_ballBaseline.fps    = snap.fps;
    m_ballBaseline.noise0 = snap.noise0;
}
#endif

// v3.4 (design §9): drop samples older than kBallAccumWindowUs — a simple
// time-bounded trim (not the exact SwingWindow ring accounting), generous
// enough to always cover the trailing window ShotProcessor freezes.
void CameraInstance::trimBallAccum()
{
    if (m_ballAccum.empty()) return;
    const qint64 cutoff = m_ballAccum.back().tUs - kBallAccumWindowUs;
    while (!m_ballAccum.empty() && m_ballAccum.front().tUs < cutoff)
        m_ballAccum.pop_front();
}

bool CameraInstance::ballLaunchInfo(qint64 &tUsOut, double &xOut, double &yOut) const
{
    if (m_ballLaunchTUs < 0) return false;
    tUsOut = m_ballLaunchTUs;
    xOut   = m_ballLaunchX;
    yOut   = m_ballLaunchY;
    return true;
}

VideoPreprocessorBase *CameraInstance::preprocessor() const { return m_preprocessor; }

// ---------------------------------------------------------------------------
// View subscription — publish/subscribe fan-out
// ---------------------------------------------------------------------------

void CameraInstance::addVideoSink(QVideoSink *sink)
{
    if (!sink || m_videoSinks.contains(sink))
        return;
    m_videoSinks.append(sink);
}

void CameraInstance::removeVideoSink(QVideoSink *sink)
{
    m_videoSinks.removeAll(sink);
    m_videoSinks.removeAll(nullptr);
}

void CameraInstance::addBayerItem(QObject *item)
{
    auto *bayer = qobject_cast<BayerVideoItem *>(item);
    if (item && !bayer) {
        ppError() << "[CameraInstance] addBayerItem: cast failed — item is" << item->metaObject()->className();
        return;
    }
    if (!bayer || m_bayerItems.contains(bayer))
        return;
    m_bayerItems.append(bayer);
}

void CameraInstance::removeBayerItem(QObject *item)
{
    m_bayerItems.removeAll(qobject_cast<BayerVideoItem *>(item));
    m_bayerItems.removeAll(nullptr);
}

void CameraInstance::setSettingsSink(QVideoSink *sink)
{
    m_settingsSink = sink;
}

void CameraInstance::selectMoveNetModel(int variant)
{
#if defined(HAVE_OPENCV) && defined(HAVE_ONNXRUNTIME)
    if (variant == m_moveNetModel || !m_poseThread)
        return;

    m_moveNetModel = variant;
    emit moveNetModelChanged();
    m_poseAvgMs = 0.0; emit poseAvgMsChanged();
    m_poseFps   = 0.0; emit poseFpsChanged();
    m_poseBackendLabel = QString(); emit poseBackendLabelChanged();

#if defined(HAVE_MOVENET)
    if (m_poseEstimator) {
        QMetaObject::invokeMethod(m_poseEstimator, "reloadModel",
                                  Qt::QueuedConnection, Q_ARG(int, variant));
    }
#endif
#else
    Q_UNUSED(variant)
#endif
}

// ---------------------------------------------------------------------------
// Recording control
// ---------------------------------------------------------------------------

void CameraInstance::startPreview()
{
    // m_captureThread is never started for a PPCP-backed instance (see
    // m_isPpcpBackend) — its own "am I ready" signal is m_videoInput existing
    // at all, not the thread that every other backend actually needs.
    if (m_previewing || (!m_isPpcpBackend && !m_captureThread->isRunning())) return;
    m_previewing = true;
    if (m_recording) return; // camera already running; pipeline already active
    m_previewOnly.store(true, std::memory_order_relaxed);
#ifdef HAVE_PPCP_TRANSPORT
    ppcpAttachIfNeeded();
#endif
    QMetaObject::invokeMethod(m_videoInput, [this]() {
        // Preview is always full sensor — the settings crop editor needs the
        // uncropped view to drag-select against. The crop applies on capture
        // connections only (startRecording).
        m_videoInput->setCropRegion(QRectF());
        if (!m_videoInput->start(m_deviceId)) {
            QMetaObject::invokeMethod(this, [this]() {
                m_previewing = false;
                m_previewOnly.store(false, std::memory_order_relaxed);
            }, Qt::QueuedConnection);
        }
    }, Qt::QueuedConnection);
}

void CameraInstance::stopPreview()
{
    if (!m_previewing) return;
    m_previewing = false;
    m_previewOnly.store(false, std::memory_order_relaxed);
    if (m_recording) return; // recording keeps camera alive
    QMetaObject::invokeMethod(m_videoInput, [this]() {
        m_videoInput->stop();
    }, Qt::QueuedConnection);
}

void CameraInstance::startRecording()
{
    if (m_recording || !m_captureThread->isRunning())
        return;

#ifdef Q_OS_MACOS
    // The permission completion handler fires on a GCD queue whenever the
    // user answers the dialog — possibly after this instance was deselected
    // and destroyed. Guard with a QPointer and use the application object as
    // the invoke context (a destroyed raw `this` is not a valid context).
    QPointer<CameraInstance> self(this);
    requestCameraPermission([self](bool granted) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, granted]() {
            if (!self)
                return;
            if (!granted) {
                ppError() << "[CameraInstance] Camera permission denied."
                           << "Grant access in System Settings → Privacy & Security → Camera.";
                return;
            }
            QMetaObject::invokeMethod(self->m_videoInput, [self]() {
                if (!self)
                    return;
                self->m_videoInput->setCropRegion(self->m_activeCropRoi);
                if (!self->m_videoInput->start(self->m_deviceId)) {
                    // Revert the optimistic recording state (non-macOS path
                    // does the same via its queued failure hop).
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [self]() {
                        if (!self) return;
                        self->m_recording = false;
                        emit self->isRecordingChanged();
                        self->setConnecting(false);
                    }, Qt::QueuedConnection);
                }
            }, Qt::QueuedConnection);
            self->m_recording = true;
            emit self->isRecordingChanged();
            self->setConnecting(true);   // connect attempt in flight
        }, Qt::QueuedConnection);
    });
#else
    m_recording = true;
    emit isRecordingChanged();

    if (m_previewing) {
        // Camera already running — promote to full recording by enabling the
        // pipeline. Accepted limitation: the device was started uncropped
        // (preview is full sensor), so until the next real connect the crop
        // is applied by the software fallback rather than hardware ROI.
        m_previewOnly.store(false, std::memory_order_relaxed);
        return;
    }

    setConnecting(true);   // entering the real device-start window

    QMetaObject::invokeMethod(m_videoInput, [this]() {
        // Prime the hardware ROI (no-op for software-cropped backends) with
        // the ctor-frozen crop before starting the device.
        m_videoInput->setCropRegion(m_activeCropRoi);
        if (m_videoInput->start(m_deviceId))
            return;

        if (m_deviceId.isEmpty()) {
            // Auto-mode only: fall back from industrial camera to Qt Multimedia webcam.
            ppError() << "[CameraInstance] Failed to start primary video input. Attempting fallback...";
            VideoInputFactory::Backend currentBackend = VideoInputFactory::backendType(m_videoInput);
            if (currentBackend == VideoInputFactory::Backend::Aravis ||
                currentBackend == VideoInputFactory::Backend::Spinnaker)
            {
                QMetaObject::invokeMethod(this, [this]() {
                    // The failed input lives on the (running) capture thread —
                    // deleting it from the main thread races its event loop.
                    // deleteLater() destroys it on its own thread.
                    m_videoInput->deleteLater();
                    m_videoInput = VideoInputFactory::create(VideoInputFactory::Backend::QtMultimedia);
                    m_videoInput->moveToThread(m_captureThread);
                    connectVideoInput();
                    emit isAravisChanged();
                    emit isSpinnakerChanged();
                    emit needsDebayerChanged();
                    // No setCropRegion needed: the QtMultimedia fallback has
                    // no hardware ROI and the software crop engages via the
                    // !supportsHardwareCrop() check in connectVideoInput().
                    QMetaObject::invokeMethod(m_videoInput, [this]() {
                        if (!m_videoInput->start()) {
                            QMetaObject::invokeMethod(this, [this]() {
                                m_recording = false;
                                emit isRecordingChanged();
                                setConnecting(false);
                            }, Qt::QueuedConnection);
                        }
                    }, Qt::QueuedConnection);
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this]() {
                    m_recording = false;
                    emit isRecordingChanged();
                    setConnecting(false);
                }, Qt::QueuedConnection);
            }
        } else {
            ppError() << "[CameraInstance] Failed to start camera:" << m_deviceDescription;
            QMetaObject::invokeMethod(this, [this]() {
                m_recording = false;
                emit isRecordingChanged();
                setConnecting(false);
            }, Qt::QueuedConnection);
        }
    }, Qt::QueuedConnection);
#endif
}

void CameraInstance::stopRecording()
{
    if (!m_recording)
        return;
    if (!m_previewing) {
        QMetaObject::invokeMethod(m_videoInput, [this]() {
            m_videoInput->stop();
        }, Qt::QueuedConnection);
    } else {
        // Demote back to preview-only — camera stays running but pipeline suppressed.
        m_previewOnly.store(true, std::memory_order_relaxed);
    }
    m_recording = false;
    emit isRecordingChanged();
    setConnecting(false);   // user cancelled before the device came up
    m_poseKeypoints.clear();
    emit poseKeypointsChanged();
    if (m_ballDetected) {
        m_ballDetected = false;
        m_ballX = m_ballY = m_ballRadius = 0.0;
        emit ballDetectedChanged();
    }
    m_ballWindow.clear();
    m_ballPresentCount = 0;
    if (m_ballPresent) {
        m_ballPresent = false;
        emit ballPresentChanged(false);
    }
    if (m_ballPresencePercent != 0.0) {
        m_ballPresencePercent = 0.0;
        emit ballPresencePercentChanged();
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void CameraInstance::drainDisplayFrame()
{
    // Reset pending flag BEFORE consuming the frame so that any videoFrameReady
    // arriving in this window can schedule a fresh drain (prevents frames getting
    // permanently stuck when they arrive mid-drain).
    m_displayFramePending.store(false, std::memory_order_release);

    QVideoFrame f;
    {
        QMutexLocker lk(&m_latestFrameMutex);
        f = m_latestDisplayFrame;
        m_latestDisplayFrame = QVideoFrame();
    }

    if (m_replaying)  // replay frames are pushed directly; suppress live feed
        return;

    // The delivered frame is the authority on the displayed size — it carries
    // the cropped dimensions when a crop is active, whereas the capabilities
    // query reports the camera's (pre-crop) delivery size. Always correct any
    // mismatch so tile aspect/layout follows what is actually shown.
    if (f.isValid() && f.width() > 0 && f.height() > 0
        && (f.width() != m_frameWidth || f.height() != m_frameHeight)) {
        m_frameWidth  = f.width();
        m_frameHeight = f.height();
        emit frameSizeChanged();
    }
    if (f.isValid())
        m_lastDeliveredFrame = f;   // ground truth for updateBufferDescriptor()
    onVideoFrame(f);

    // Stamp the buffer descriptor's pixel format + resolution from the first real
    // frame. Single source of truth; cheap no-op after the first successful stamp.
    if (f.isValid() && !m_bufferDescriptorStamped)
        updateBufferDescriptor();
    else if (f.isValid() && m_videoInput)
        refreshExposure(m_videoInput->lastMeasuredExposureUs(),
                        m_videoInput->lastExposureAutoMode());  // no-op for fps-derived webcams
}

void CameraInstance::drainRawFrame()
{
    // Same ordering as drainDisplayFrame: reset pending flag first.
    m_displayFramePending.store(false, std::memory_order_release);

    RawVideoFrame raw;
    {
        QMutexLocker lk(&m_latestRawFrameMutex);
        raw              = m_latestRawFrame;
        m_latestRawFrame = RawVideoFrame();
    }

    // Keep the buffer descriptor in sync with the delivered raw frames even
    // while replaying — payloads keep being published during replay.
    if (!raw.isNull()) {
        stampBufferDescriptorFromRaw(raw);
        refreshExposure(raw.exposureUs, raw.exposureAuto);  // track auto-exposure drift
    }

    if (m_replaying || raw.isNull())  // suppress live feed during replay
        return;

    // Delivered raw frame is the authority on the displayed size (cropped
    // dims when a crop is active) — same rule as drainDisplayFrame.
    if (raw.width > 0 && raw.height > 0
        && (raw.width != m_frameWidth || raw.height != m_frameHeight)) {
        m_frameWidth  = raw.width;
        m_frameHeight = raw.height;
        emit frameSizeChanged();
    }

    static bool logged = false;
    if (!logged) {
        ppDebug() << "[CameraInstance] first raw Bayer frame:" << raw.width << "x" << raw.height
                 << "pattern" << static_cast<int>(raw.pattern)
                 << "bayerItems" << m_bayerItems.size();
        logged = true;
    }

    m_bayerItems.removeAll(nullptr);
    for (const auto &item : std::as_const(m_bayerItems))
        item->updateFrame(raw);

    // Deliver greyscale Y8 preview to the settings sink (Bayer pattern as luma).
    if (m_settingsSink && !raw.isNull()) {
        QVideoFrameFormat fmt(QSize(raw.width, raw.height), QVideoFrameFormat::Format_Y8);
        QVideoFrame yFrame(fmt);
        if (yFrame.map(QVideoFrame::WriteOnly)) {
            const auto *src = reinterpret_cast<const uchar *>(raw.data.constData());
            uchar *dst = yFrame.bits(0);
            // Raw data is always 8-bit packed (BayerRG8 etc., stride == width)
            // from the ring buffer, but Qt may allocate the Y8 frame with
            // padded rows — copy row-by-row honouring the destination stride
            // or the preview shears.
            const int dstStride = yFrame.bytesPerLine(0);
            if (dstStride == raw.width) {
                memcpy(dst, src, static_cast<size_t>(raw.width) * raw.height);
            } else {
                for (int y = 0; y < raw.height; ++y)
                    memcpy(dst + y * dstStride, src + y * raw.width, raw.width);
            }
            yFrame.unmap();
            m_settingsSink->setVideoFrame(yFrame);
        }
    }
}

void CameraInstance::onVideoFrame(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;
    // Publish to every subscribed view. QVideoFrame is implicitly shared —
    // each setVideoFrame() is a handle assignment, not a copy. Prune sinks
    // that were destroyed without unsubscribing (QPointer nulls them).
    m_videoSinks.removeAll(nullptr);
    for (const auto &sink : std::as_const(m_videoSinks))
        sink->setVideoFrame(frame);
    if (m_settingsSink)
        m_settingsSink->setVideoFrame(frame);
}

#ifdef HAVE_OPENCV
void CameraInstance::onPoseEstimated(const PoseResult &result)
{
    if (m_replaying) return;

    // A result can already be in flight (queued from the pose thread) when
    // setPoseEnabled(false) clears the keypoints — dropping it here stops the
    // last skeleton from reappearing and freezing on screen.
    if (!m_poseEnabled) return;

    QVariantList kps;
    kps.reserve(PoseResult::kNumKeypoints);
    for (const auto &kp : result.keypoints) {
        QVariantMap m;
        m[QStringLiteral("x")]     = static_cast<double>(kp.x);
        m[QStringLiteral("y")]     = static_cast<double>(kp.y);
        m[QStringLiteral("score")] = static_cast<double>(kp.score);
        kps.append(m);
    }
    m_poseKeypoints = kps;
    emit poseKeypointsChanged();
}
#endif

void CameraInstance::onVideoError(const QString &message)
{
    ppWarn() << "[Video]" << message;
    // A device error during a connect attempt ends the in-flight window.
    setConnecting(false);
    if (m_lastPreviewError != message) {
        m_lastPreviewError = message;
        emit lastPreviewErrorChanged();
    }
}

void CameraInstance::onPreprocessStats(double avgMs)
{
    if (qFuzzyCompare(m_preprocessAvgMs, avgMs))
        return;
    m_preprocessAvgMs = avgMs;
    emit preprocessAvgMsChanged();
}

void CameraInstance::onPoseStats(double avgMs, double fps)
{
    if (!qFuzzyCompare(m_poseAvgMs, avgMs)) { m_poseAvgMs = avgMs; emit poseAvgMsChanged(); }
    if (!qFuzzyCompare(m_poseFps,   fps))   { m_poseFps   = fps;   emit poseFpsChanged();   }
}

void CameraInstance::onPoseBackendReady(const QString &label)
{
    if (m_poseBackendLabel == label)
        return;
    m_poseBackendLabel = label;
    emit poseBackendLabelChanged();
}

// ---------------------------------------------------------------------------
// Replay support
// ---------------------------------------------------------------------------

static QVideoFrameFormat::PixelFormat toQtPixelFormat(pinpoint::PixelFormat fmt)
{
    switch (fmt) {
    case pinpoint::PixelFormat::NV12:    return QVideoFrameFormat::Format_NV12;
    case pinpoint::PixelFormat::YUYV:    return QVideoFrameFormat::Format_YUYV;
    case pinpoint::PixelFormat::UYVY:    return QVideoFrameFormat::Format_UYVY;
    case pinpoint::PixelFormat::YUV420P: return QVideoFrameFormat::Format_YUV420P;
    case pinpoint::PixelFormat::BGRA32:  return QVideoFrameFormat::Format_BGRA8888;
    default:                              return QVideoFrameFormat::Format_Invalid;
    }
}

// Inverse of toQtPixelFormat: derive the buffer pixel format from an actual
// delivered frame. This is the single source of truth for what the backend
// produces — the descriptor is stamped from the frame itself (see
// updateBufferDescriptor), so no backend hardcodes its format. All 32-bit packed
// RGB variants map to BGRA32: replay reconstructs them as Format_BGRA8888 and
// the stored bytes already carry the correct channel order.
static pinpoint::PixelFormat pinpointFromQtFormat(QVideoFrameFormat::PixelFormat fmt)
{
    switch (fmt) {
    case QVideoFrameFormat::Format_NV12:                    return pinpoint::PixelFormat::NV12;
    case QVideoFrameFormat::Format_YUYV:                    return pinpoint::PixelFormat::YUYV;
    case QVideoFrameFormat::Format_UYVY:                    return pinpoint::PixelFormat::UYVY;
    case QVideoFrameFormat::Format_YUV420P:                 return pinpoint::PixelFormat::YUV420P;
    case QVideoFrameFormat::Format_ARGB8888:
    case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
    case QVideoFrameFormat::Format_XRGB8888:
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_BGRA8888_Premultiplied:
    case QVideoFrameFormat::Format_BGRX8888:
    case QVideoFrameFormat::Format_ABGR8888:
    case QVideoFrameFormat::Format_XBGR8888:
    case QVideoFrameFormat::Format_RGBA8888:
    case QVideoFrameFormat::Format_RGBX8888:                return pinpoint::PixelFormat::BGRA32;
    default:                                                return pinpoint::PixelFormat::Unknown;
    }
}

pinpoint::SourceId CameraInstance::sourceId() const { return m_sourceId; }
bool               CameraInstance::isReplaying() const { return m_replaying; }

void CameraInstance::setReplaying(bool replaying)
{
    if (m_replaying == replaying) return;
    m_replaying = replaying;
    if (replaying) {
        m_poseKeypoints.clear();
        emit poseKeypointsChanged();
        // Drop the live ball detection too, so the live ballCircle overlay hides
        // during replay (the replay overlay draws the recorded ball instead) —
        // mirrors the pose-keypoint clear above. onBallDetected already early-outs
        // while replaying, so no stale live update revives it.
        if (m_ballDetected) {
            m_ballDetected = false;
            m_ballX = m_ballY = m_ballRadius = 0.0;
            emit ballDetectedChanged();
        }
    }
    emit isReplayingChanged();
}

void CameraInstance::displayReplayFrame(const std::byte *data, size_t bytes,
                                          int w, int h, pinpoint::PixelFormat fmt,
                                          const std::array<uint32_t, 4> &srcStrides)
{
    if (!data || bytes == 0) return;

    // The replayed frame is what's visible — keep the reported frame size in
    // sync so tile aspect/layout follows the replay (dims come from the
    // stored descriptor, i.e. the cropped capture size). Runs on the main
    // thread (invoked from CameraManager's replay timer).
    if (w > 0 && h > 0 && (w != m_frameWidth || h != m_frameHeight)) {
        m_frameWidth  = w;
        m_frameHeight = h;
        emit frameSizeChanged();
    }

    const bool isBayer = (fmt == pinpoint::PixelFormat::BayerRG8  ||
                          fmt == pinpoint::PixelFormat::BayerRG12 ||
                          fmt == pinpoint::PixelFormat::BayerRG16 ||
                          fmt == pinpoint::PixelFormat::BayerBG8  ||
                          fmt == pinpoint::PixelFormat::BayerGR8  ||
                          fmt == pinpoint::PixelFormat::BayerGB8  ||
                          fmt == pinpoint::PixelFormat::BayerGB16);

    if (isBayer) {
        m_bayerItems.removeAll(nullptr);
        if (m_bayerItems.isEmpty()) return;
        RawVideoFrame raw;
        raw.width   = w;
        raw.height  = h;
        raw.pattern = (fmt == pinpoint::PixelFormat::BayerBG8)
                    ? RawVideoFrame::BayerPattern::BG
                    : (fmt == pinpoint::PixelFormat::BayerGR8)
                    ? RawVideoFrame::BayerPattern::GR
                    : (fmt == pinpoint::PixelFormat::BayerGB8  ||
                       fmt == pinpoint::PixelFormat::BayerGB16)
                    ? RawVideoFrame::BayerPattern::GB
                    : RawVideoFrame::BayerPattern::RG;
        raw.data    = QByteArray(reinterpret_cast<const char *>(data),
                                 static_cast<qsizetype>(bytes));
        for (const auto &item : std::as_const(m_bayerItems))
            item->updateFrame(raw);
    } else {
        m_videoSinks.removeAll(nullptr);
        if (m_videoSinks.isEmpty()) return;
        const auto qtFmt = toQtPixelFormat(fmt);
        if (qtFmt == QVideoFrameFormat::Format_Invalid) return;

        QVideoFrameFormat frameFormat(QSize(w, h), qtFmt);
        QVideoFrame replayFrame(frameFormat);
        if (!replayFrame.map(QVideoFrame::WriteOnly)) return;

        // Restore all planes from the sequentially-stored buffer.
        // publishFrameToBuffer stores planes in order: plane0 bytes, plane1
        // bytes, … each with the CAPTURE frame's stride (descriptor
        // plane_strides). Qt may allocate this replay frame with different
        // row alignment, so copy row-by-row honouring both strides — a flat
        // memcpy shears every row and misplaces the chroma plane whenever
        // the two allocations disagree.
        const auto *src = reinterpret_cast<const uint8_t *>(data);
        size_t offset = 0;
        for (int p = 0; p < replayFrame.planeCount(); ++p) {
            const size_t dstStride  = static_cast<size_t>(replayFrame.bytesPerLine(p));
            const size_t planeBytes = static_cast<size_t>(replayFrame.mappedBytes(p));
            const size_t srcStride  =
                (p < static_cast<int>(srcStrides.size()) && srcStrides[static_cast<size_t>(p)])
                    ? srcStrides[static_cast<size_t>(p)] : dstStride;
            const size_t rows       = dstStride ? planeBytes / dstStride : 0;
            const size_t copyBytes  = std::min(srcStride, dstStride);
            uint8_t *dst = replayFrame.bits(p);
            if (srcStride == dstStride) {
                const size_t available = offset < bytes ? bytes - offset : 0;
                std::memcpy(dst, src + offset, std::min(planeBytes, available));
            } else {
                for (size_t r = 0; r < rows; ++r) {
                    const size_t srcOff = offset + r * srcStride;
                    if (srcOff + copyBytes > bytes) break;
                    std::memcpy(dst + r * dstStride, src + srcOff, copyBytes);
                }
            }
            offset += srcStride * rows;
        }

        replayFrame.unmap();
        for (const auto &sink : std::as_const(m_videoSinks))
            sink->setVideoFrame(replayFrame);
    }
}

// ---------------------------------------------------------------------------
// EventBuffer descriptor correction (stamped once from the first real frame)
// ---------------------------------------------------------------------------

// Correct the source descriptor's pixel format and resolution to match the
// frame the backend is actually delivering. The frame is the single source of
// truth: its surfaceFormat()/dimensions are exactly how the bytes published into
// the ring are laid out, so swing replay decodes with the same format/size it was
// stored with — no per-backend hardcoding, and robust to any future format.
//
// Sets a CameraFormat's exposure fields: a measured per-frame value when one is
// available (source records whether auto-exposure was active), otherwise the
// 1/(2*fps) derived fallback, otherwise left Unknown. Single home for the formula.
static void applyExposure(pinpoint::CameraFormat &cfmt, double exposureUs,
                          int exposureAuto, double fps)
{
    if (exposureUs > 0.0) {
        cfmt.exposure_us     = exposureUs;
        cfmt.exposure_source = (exposureAuto == 1) ? pinpoint::ExposureSource::MeasuredAuto
                                                   : pinpoint::ExposureSource::Measured;
    } else if (fps > 0.0) {
        cfmt.exposure_us     = 0.5e6 / fps;   // 1/(2*fps) in microseconds
        cfmt.exposure_source = pinpoint::ExposureSource::Derived;
    }   // else leave Unknown / 0.0
}

// Called from drainDisplayFrame() once the first real frame has been delivered
// (held in m_lastDeliveredFrame — the subscriber list may legitimately be empty).
// The ring slot was already sized conservatively at registerSource(); this only
// updates metadata (updateSourceFormat does not touch ring memory).
void CameraInstance::updateBufferDescriptor()
{
    if (!m_eventBuffer || m_sourceId == pinpoint::kInvalidSourceId) return;
    if (m_bufferDescriptorStamped) return;

    const QVideoFrame f = m_lastDeliveredFrame;
    if (!f.isValid()) return;  // no real frame yet — never stamp a guess

    const QVideoFrameFormat   sf  = f.surfaceFormat();
    const pinpoint::PixelFormat pixfmt = pinpointFromQtFormat(sf.pixelFormat());
    const int w = f.width();
    const int h = f.height();
    if (w <= 0 || h <= 0 || pixfmt == pinpoint::PixelFormat::Unknown) return;

    const auto backend = VideoInputFactory::backendType(m_videoInput);
    pinpoint::FormatDescriptor fd;
    fd.device = (backend == VideoInputFactory::Backend::Aravis ||
                 backend == VideoInputFactory::Backend::Spinnaker)
                ? pinpoint::DeviceKind::Camera_GenICam
                : (backend == VideoInputFactory::Backend::AppleAVFoundation
                   ? pinpoint::DeviceKind::Camera_AVFoundation
                   : pinpoint::DeviceKind::Camera_UVC);

    pinpoint::CameraFormat cfmt{};
    cfmt.pixel_format = pixfmt;
    cfmt.width        = static_cast<uint32_t>(w);
    cfmt.height       = static_cast<uint32_t>(h);
    cfmt.max_payload_bytes     = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
    cfmt.typical_payload_bytes = cfmt.max_payload_bytes;
    const double fps           = sf.streamFrameRate() > 0.0 ? sf.streamFrameRate() : 30.0;
    cfmt.fps_numerator   = static_cast<uint32_t>(fps * 1000.0);
    cfmt.fps_denominator = 1000;

    // Exposure: QVideoFrame carries none, so poll the backend side channel
    // (industrial Mono/RGB path) and fall back to fps-derived for webcams.
    const double expUs   = m_videoInput ? m_videoInput->lastMeasuredExposureUs() : 0.0;
    const int    expAuto = m_videoInput ? m_videoInput->lastExposureAutoMode()   : -1;
    applyExposure(cfmt, expUs, expAuto, fps);

    // Plane strides exactly as delivered — and therefore exactly as stored:
    // publishFrameToBuffer copies mappedBytes(p) per plane, INCLUDING any row
    // padding the backend allocates (bytesPerLine != width*bpp on padded
    // backends). Swing export and replay must decode with these strides, not
    // a tight width*bpp assumption, or every saved/replayed frame shears.
    {
        QVideoFrame strideProbe = f;
        if (strideProbe.map(QVideoFrame::ReadOnly)) {
            const int planes = std::min(strideProbe.planeCount(),
                                        static_cast<int>(cfmt.plane_strides.size()));
            for (int p = 0; p < planes; ++p)
                cfmt.plane_strides[static_cast<size_t>(p)] =
                    static_cast<uint32_t>(strideProbe.bytesPerLine(p));
            strideProbe.unmap();
        }
    }
    fd.format = cfmt;

    m_stampedDescriptor = fd;                     // basis for refreshExposure()
    m_eventBuffer->updateSourceFormat(m_sourceId, fd);
    m_bufferDescriptorStamped = true;

    ppInfo() << "[CameraInstance] buffer descriptor stamped from frame:"
             << m_deviceDescription << w << "x" << h << "@" << fps << "fps"
             << "qtFmt:" << static_cast<int>(sf.pixelFormat())
             << "-> pinpointFmt:" << static_cast<int>(pixfmt);
}

// Raw-Bayer counterpart of updateBufferDescriptor(). The Spinnaker raw path
// never delivers a QVideoFrame, so the descriptor would otherwise keep the
// registration-time dimensions — which differ from the delivered frames once
// the ROI is applied in start(), making swing export/replay decode every
// payload with the wrong width (or skip them all as too short). Re-stamps if
// the delivered dims/pattern ever change so the descriptor always matches the
// payloads being published. Called from drainRawFrame() (main thread).
void CameraInstance::stampBufferDescriptorFromRaw(const RawVideoFrame &raw)
{
    if (!m_eventBuffer || m_sourceId == pinpoint::kInvalidSourceId) return;
    if (raw.width == m_stampedRawWidth && raw.height == m_stampedRawHeight
        && raw.pattern == m_stampedRawPattern)
        return;

    pinpoint::PixelFormat pixfmt;
    switch (raw.pattern) {
    case RawVideoFrame::BayerPattern::RG: pixfmt = pinpoint::PixelFormat::BayerRG8; break;
    case RawVideoFrame::BayerPattern::BG: pixfmt = pinpoint::PixelFormat::BayerBG8; break;
    case RawVideoFrame::BayerPattern::GR: pixfmt = pinpoint::PixelFormat::BayerGR8; break;
    case RawVideoFrame::BayerPattern::GB: pixfmt = pinpoint::PixelFormat::BayerGB8; break;
    default: return;
    }

    pinpoint::FormatDescriptor fd;
    fd.device = pinpoint::DeviceKind::Camera_GenICam;   // raw Bayer is GenICam-only

    pinpoint::CameraFormat cfmt{};
    cfmt.pixel_format = pixfmt;
    cfmt.width        = static_cast<uint32_t>(raw.width);
    cfmt.height       = static_cast<uint32_t>(raw.height);
    // Payloads are packed 8-bit Bayer (stride == width — see raw_video_frame.h).
    cfmt.max_payload_bytes     = cfmt.width * cfmt.height;
    cfmt.typical_payload_bytes = cfmt.max_payload_bytes;
    cfmt.plane_strides[0]      = cfmt.width;   // packed — consumers must not guess
    const double fps = m_configuredFps > 0.0 ? m_configuredFps : 30.0;
    cfmt.fps_numerator   = static_cast<uint32_t>(fps * 1000.0);
    cfmt.fps_denominator = 1000;
    // Exposure measured from the frame's chunk data (Spinnaker), else fps-derived.
    applyExposure(cfmt, raw.exposureUs, raw.exposureAuto, fps);
    fd.format = cfmt;

    m_stampedDescriptor = fd;                     // basis for refreshExposure()
    m_eventBuffer->updateSourceFormat(m_sourceId, fd);
    m_bufferDescriptorStamped = true;   // suppress the QVideoFrame-path stamp
    m_stampedRawWidth   = raw.width;
    m_stampedRawHeight  = raw.height;
    m_stampedRawPattern = raw.pattern;

    ppInfo() << "[CameraInstance] buffer descriptor stamped from raw frame:"
             << m_deviceDescription << raw.width << "x" << raw.height
             << "@" << fps << "fps pattern:" << static_cast<int>(raw.pattern)
             << "-> pinpointFmt:" << static_cast<int>(pixfmt);
}

// Refresh just the exposure fields of the already-stamped descriptor when a
// materially different exposure arrives (auto-exposure drifts between the first
// preview frame and the recorded swing). Metadata-only and main-thread; the
// swingWindowLive() guard preserves the invariant that a descriptor is immutable
// while a shot-analysis worker reads it through formatOf()'s live reference.
void CameraInstance::refreshExposure(double exposureUs, int exposureAutoMode)
{
    if (!m_eventBuffer || m_sourceId == pinpoint::kInvalidSourceId
        || !m_bufferDescriptorStamped)
        return;
    if (exposureUs <= 0.0)                       // no measured value to apply
        return;
    if (m_eventBuffer->swingWindowLive())        // never mutate what a worker reads
        return;

    auto *cf = std::get_if<pinpoint::CameraFormat>(&m_stampedDescriptor.format);
    if (!cf)
        return;
    const double old = cf->exposure_us;
    if (old > 0.0 && std::abs(exposureUs - old) < 0.15 * old)  // material change only
        return;

    cf->exposure_us     = exposureUs;
    cf->exposure_source = (exposureAutoMode == 1) ? pinpoint::ExposureSource::MeasuredAuto
                                                  : pinpoint::ExposureSource::Measured;
    m_eventBuffer->updateSourceFormat(m_sourceId, m_stampedDescriptor);
}

// ---------------------------------------------------------------------------
// EventBuffer publish helpers (called on the capture thread via DirectConnection)
// ---------------------------------------------------------------------------

void CameraInstance::publishFrameToBuffer(const QVideoFrame &frame, qint64 frameTUs)
{
    if (!frame.isValid())
        return;

    QVideoFrame mutable_frame = frame;  // QVideoFrame is ref-counted; copy is cheap
    if (!mutable_frame.map(QVideoFrame::ReadOnly))
        return;

    // Sum all planes — multi-plane formats (NV12, YUV420P) have separate luma
    // and chroma planes; storing only bits(0) loses the chroma plane and causes
    // green frames on replay.
    size_t totalBytes = 0;
    for (int p = 0; p < mutable_frame.planeCount(); ++p)
        totalBytes += static_cast<size_t>(mutable_frame.mappedBytes(p));

    // Validate the size BEFORE acquiring a write slot: an acquired-but-never-
    // published slot stays odd-generation, which the merger treats as
    // mid-write, stalling this source's timeline indexing for a full ring
    // lap. The usual cause is the ring slot being sized smaller than the
    // delivered frame (e.g. capacity sized for the enumerated NV12/YUYV
    // format while the macOS AVFoundation backend delivers 32-bpp ARGB32).
    // Log once: at the capture rate this would otherwise flood the bounded
    // log and bury every other entry.
    const size_t capacity = m_eventBuffer->getSlotCapacity(m_sourceId);
    if (totalBytes == 0 || totalBytes > capacity) {
        if (totalBytes > capacity &&
            !m_publishDropLogged.exchange(true, std::memory_order_relaxed)) {
            ppError() << "[CameraInstance] dropping every frame from the event buffer:"
                      << totalBytes << "byte frame does not fit the" << capacity
                      << "byte ring slot (planes:" << mutable_frame.planeCount()
                      << "). No data will be captured and swing replay will be empty."
                      << "This is logged once per session.";
        }
        mutable_frame.unmap();
        return;
    }

    auto slot = m_eventBuffer->acquireWriteSlot(m_sourceId);
    if (slot.valid) {
        auto *dst = reinterpret_cast<uint8_t *>(slot.data);
        size_t offset = 0;
        for (int p = 0; p < mutable_frame.planeCount(); ++p) {
            const size_t planeBytes = static_cast<size_t>(mutable_frame.mappedBytes(p));
            std::memcpy(dst + offset, mutable_frame.bits(p), planeBytes);
            offset += planeBytes;
        }
        *slot.bytes_written = static_cast<uint32_t>(totalBytes);
        *slot.timestamp_us  = frameTUs;   // capture-thread instant, shared with the ball throttle
        m_eventBuffer->publish(m_sourceId, slot.sequence);
    }

    mutable_frame.unmap();
}

void CameraInstance::publishRawFrameToBuffer(const RawVideoFrame &frame, qint64 frameTUs)
{
    if (frame.isNull())
        return;

    // Same pre-acquire validation as publishFrameToBuffer: never abandon an
    // acquired slot (odd generation stalls the merger), and never drop
    // silently.
    const size_t size = static_cast<size_t>(frame.data.size());
    const size_t capacity = m_eventBuffer->getSlotCapacity(m_sourceId);
    if (size == 0 || size > capacity) {
        if (size > capacity &&
            !m_publishDropLogged.exchange(true, std::memory_order_relaxed)) {
            ppError() << "[CameraInstance] dropping every raw frame from the event buffer:"
                      << size << "byte frame does not fit the" << capacity
                      << "byte ring slot. No data will be captured and swing"
                      << "replay will be empty. This is logged once per session.";
        }
        return;
    }
    auto slot = m_eventBuffer->acquireWriteSlot(m_sourceId);
    if (slot.valid) {
        std::memcpy(slot.data, frame.data.constData(), size);
        *slot.bytes_written = static_cast<uint32_t>(size);
        *slot.timestamp_us  = frameTUs;   // capture-thread instant, shared with the ball throttle
        m_eventBuffer->publish(m_sourceId, slot.sequence);
    }
}
