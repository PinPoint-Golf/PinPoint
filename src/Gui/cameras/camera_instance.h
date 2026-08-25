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

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QRectF>
#include <QTimer>
#include <QVariantList>
#include <QVideoFrame>
#include <QVideoSink>
#include <array>
#include <atomic>
#include <deque>

#include "device_enumerator.h"
#include "format_descriptor.h"
#include "raw_video_frame.h"
#include "ting_player.h"
#include "types.h"

namespace pinpoint { class EventBuffer; }

class AppSettings;
class QThread;
class QVideoFrame;
class QVideoSink;
class VideoInputBase;
class VideoPreprocessorBase;
class BayerVideoItem;
#ifdef HAVE_PPCP_TRANSPORT
class PpcpHostService;
#endif

#ifdef HAVE_OPENCV
#include "pose_estimator_base.h"
#include "ball_detector.h"
class FrameThrottle;
#endif

class CameraInstance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CameraInstance is created by CameraManager")

    Q_PROPERTY(bool isRecording READ isRecording NOTIFY isRecordingChanged)
    // True while a connect attempt is in flight (device start posted, not yet
    // confirmed Active). Cleared on first Active frame, start failure, video
    // error, or stop. Drives the Connect-button "connecting" animation.
    Q_PROPERTY(bool isConnecting READ isConnecting NOTIFY isConnectingChanged)
    Q_PROPERTY(bool isAravis READ isAravis NOTIFY isAravisChanged)
    Q_PROPERTY(bool isSpinnaker READ isSpinnaker NOTIFY isSpinnakerChanged)
    Q_PROPERTY(bool needsDebayer READ needsDebayer NOTIFY needsDebayerChanged)
    Q_PROPERTY(double preprocessAvgMs READ preprocessAvgMs NOTIFY preprocessAvgMsChanged)
    Q_PROPERTY(double cameraFps READ cameraFps NOTIFY cameraFpsChanged)
    Q_PROPERTY(double poseAvgMs READ poseAvgMs NOTIFY poseAvgMsChanged)
    Q_PROPERTY(double poseFps   READ poseFps   NOTIFY poseFpsChanged)
    Q_PROPERTY(QString poseBackendLabel READ poseBackendLabel NOTIFY poseBackendLabelChanged)
    // moveNetModel is the active pose-model selector: 0=Lightning, 1=Thunder.
    Q_PROPERTY(int moveNetModel READ moveNetModel NOTIFY moveNetModelChanged)
    Q_PROPERTY(bool moveNetThunderAvailable READ moveNetThunderAvailable CONSTANT)
    Q_PROPERTY(QVariantList poseKeypoints READ poseKeypoints NOTIFY poseKeypointsChanged)
    // Per-method pipeline enables — each gates inference inside its consumer
    // (estimatePose / detect early-out that still releases the FrameThrottle),
    // so they can be configured independently per rail screen.
    Q_PROPERTY(bool poseEnabled READ poseEnabled WRITE setPoseEnabled NOTIFY poseEnabledChanged)
    Q_PROPERTY(bool ballEnabled READ ballEnabled WRITE setBallEnabled NOTIFY ballEnabledChanged)
    Q_PROPERTY(QString deviceDescription  READ deviceDescription  CONSTANT)
    Q_PROPERTY(QString deviceSerialNumber READ deviceSerialNumber CONSTANT)
    // Same composition as CameraManager::cameraKey() — THE identity for
    // matching an instance to a cameraList entry in QML. Serial numbers alone
    // collide ("" === "") for UVC webcams that don't report one.
    Q_PROPERTY(QString cameraKey          READ cameraKey          CONSTANT)
    Q_PROPERTY(QString deviceAlias        READ deviceAlias        NOTIFY deviceAliasChanged)
    Q_PROPERTY(int  perspective READ perspective NOTIFY perspectiveChanged)
    Q_PROPERTY(bool isMirrored  READ isMirrored  NOTIFY isMirroredChanged)
    Q_PROPERTY(QRectF roi     READ roi     NOTIFY roiChanged)
    Q_PROPERTY(QRectF cropRoi READ cropRoi NOTIFY cropRoiChanged)
    Q_PROPERTY(bool   ballDetected       READ ballDetected       NOTIFY ballDetectedChanged)
    Q_PROPERTY(double ballX              READ ballX              NOTIFY ballDetectedChanged)
    Q_PROPERTY(double ballY              READ ballY              NOTIFY ballDetectedChanged)
    Q_PROPERTY(double ballRadius         READ ballRadius         NOTIFY ballDetectedChanged)
    Q_PROPERTY(double ballPresencePercent READ ballPresencePercent NOTIFY ballPresencePercentChanged)
    Q_PROPERTY(bool   ballPresent        READ ballPresent        NOTIFY ballPresentChanged)
    Q_PROPERTY(double ballAvgMs          READ ballAvgMs          NOTIFY ballAvgMsChanged)
    Q_PROPERTY(int    frameWidth         READ frameWidth          NOTIFY frameSizeChanged)
    Q_PROPERTY(int    frameHeight        READ frameHeight         NOTIFY frameSizeChanged)
    Q_PROPERTY(double configuredFps     READ configuredFps       NOTIFY frameSizeChanged)
    Q_PROPERTY(bool   isReplaying       READ isReplaying         NOTIFY isReplayingChanged)
    // The message from the most recent failed start()/preview attempt — e.g.
    // "PPCP camera: no peer attached" while the owning phone isn't connected,
    // which is an ordinary state for that backend, not a fault.  Empty once a
    // start has succeeded since.  Settings -> Cameras' ROI preview is the
    // first consumer (a PPCP camera's video tile was silent otherwise).
    Q_PROPERTY(QString lastPreviewError READ lastPreviewError NOTIFY lastPreviewErrorChanged)

public:
    // Perspective values — matches the perspective badge in PpCameraFrame.qml.
    // Registered with QML (CameraInstance.FaceOn etc.) — use the enum names in
    // QML, never the raw integers.
    enum Perspective { None = 0, DownTheLine = 1, FaceOn = 2, Other = 3 };
    Q_ENUM(Perspective)

    explicit CameraInstance(QObject *parent = nullptr);
    explicit CameraInstance(const Device &device,
                             pinpoint::EventBuffer *buffer = nullptr,
                             AppSettings *appSettings = nullptr,
                             QObject *parent = nullptr);
    ~CameraInstance() override;

    bool   isRecording() const;
    bool   isConnecting() const;
    bool   isAravis() const;
    bool   isSpinnaker() const;
    bool   needsDebayer() const;
    double  preprocessAvgMs() const;
    double  cameraFps() const;
    double  poseAvgMs() const;
    double  poseFps() const;
    QString poseBackendLabel() const;
    int     moveNetModel() const;
    bool    moveNetThunderAvailable() const;
    QVariantList poseKeypoints() const;
    bool    poseEnabled() const;
    void    setPoseEnabled(bool on);
    bool    ballEnabled() const;
    void    setBallEnabled(bool on);
    QString deviceDescription()   const;
    QString deviceSerialNumber()  const;
    QString cameraKey()           const;
    QString deviceAlias()         const;
    void    setDeviceAlias(const QString &alias);
    int     perspective() const;
    bool    isMirrored()  const;
    QRectF  roi()          const;
    QRectF  cropRoi()      const;
    bool    ballDetected()        const;
    double  ballX()               const;
    double  ballY()               const;
    double  ballRadius()          const;
    double  ballPresencePercent() const;
    bool    ballPresent()         const;
    double  ballAvgMs()           const;

    // v3.4 (design §9): per-frame ball-detector accumulator (plain POD, no
    // Analysis-layer dependency — mirrors how poseKeypoints() is live-only and
    // never written to a buffer). Read by ShotProcessor on the UI thread when
    // building the export/analysis jobs. Bounded to the last kBallAccumWindowUs
    // — comfortably covers the SwingWindow's own trailing ring, not the whole
    // session.
    struct BallAccumSample { qint64 tUs = 0; bool found = false; float x = 0.f, y = 0.f, r = 0.f, conf = 0.f; };
    const std::deque<BallAccumSample> &ballSamples() const { return m_ballAccum; }
    // Last struck-ball launch within the accumulator window. Returns false
    // (outputs untouched) when no launch has been recorded.
    bool ballLaunchInfo(qint64 &tUsOut, double &xOut, double &yOut) const;

    // cv-free cache of the detector's learned empty-mat baseline B (mirrors the
    // BallAccumSample pattern above — plain POD, no Analysis-layer dependency).
    // Refreshed from BallDetector::baselineReady(); read by ShotProcessor on the
    // UI thread at export time. Cleared (not merely stale) whenever the live
    // baseline is invalidated — see relearnBallBaseline() / setRoi().
    struct BallBaselineCache {
        QByteArray blob;   // row-major float32, w*h*4 bytes
        int w = 0, h = 0;  // ROI px dims at seed time
        QRectF roi;        // full-frame normalized ROI B was seeded over
        double rHat = 0.0, fps = 0.0, noise0 = 1.0;
        bool valid() const { return !blob.isEmpty() && w > 0 && h > 0; }
    };
    const BallBaselineCache &ballBaseline() const { return m_ballBaseline; }
    int     frameWidth()          const;
    int     frameHeight()         const;
    double  configuredFps()       const;
    bool    isReplaying()         const;
    QString lastPreviewError()    const { return m_lastPreviewError; }
    pinpoint::SourceId sourceId() const;

#ifdef HAVE_PPCP_TRANSPORT
    // Set by CameraManager right after construction, forwarded from its own
    // setPpcpHostService().  Only Backend::Ppcp instances ever consult it (see
    // ppcpAttachIfNeeded()) — harmless to set on any other backend.
    void setPpcpHostService(PpcpHostService *svc) { m_ppcpHostService = svc; }
#endif

    // Called by CameraManager only — not Q_INVOKABLE so QML cannot bypass.
    void stopCapture();       // Synchronously stops the capture thread; call before deregisterFromBuffer()
    void setPerspective(int p);
    void setIsMirrored(bool mirrored);
    void deregisterFromBuffer();
    void setReplaying(bool replaying);
    // srcStrides: the capture-time plane strides from the stored descriptor
    // (CameraFormat::plane_strides). Zero entries fall back to the replay
    // frame's own strides (legacy windows stamped before strides existed).
    void displayReplayFrame(const std::byte *data, size_t bytes, int w, int h,
                            pinpoint::PixelFormat fmt,
                            const std::array<uint32_t, 4> &srcStrides = {});

    // View subscription — the instance is the video source and PUBLISHES every
    // display/replay frame to all subscribed views. Any number of views may
    // subscribe (QVideoFrame is implicitly shared, so fan-out is just a loop of
    // handle assignments); destroyed sinks are dropped automatically (QPointer).
    // Main-thread only (all publishing happens on the main-thread drain paths).
    Q_INVOKABLE void addVideoSink(QVideoSink *sink);
    Q_INVOKABLE void removeVideoSink(QVideoSink *sink);
    Q_INVOKABLE void addBayerItem(QObject *item);
    Q_INVOKABLE void removeBayerItem(QObject *item);

    Q_INVOKABLE void setSettingsSink(QVideoSink *sink);
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void startPreview();  // start camera without ring-buffer capture (settings preview)
    Q_INVOKABLE void stopPreview();
    Q_INVOKABLE void selectMoveNetModel(int variant);
    Q_INVOKABLE void setRoi(QRectF roi);    // hitting area for ball detection
    Q_INVOKABLE void clearRoi();
    // v2: re-learn the empty-mat baseline from the next ~1 s (Option A). Call
    // with the mat empty; the temporal detector then detects a placed ball.
    Q_INVOKABLE void relearnBallBaseline();

#ifdef HAVE_OPENCV
    BallDetector *ballDetector() const { return m_ballDetector; }
#endif
    Q_INVOKABLE void setCropRoi(QRectF roi); // frame crop for storage / ring-buffer sizing
    Q_INVOKABLE void clearCropRoi();

    VideoPreprocessorBase *preprocessor() const;

signals:
    void isRecordingChanged();
    void isConnectingChanged();
    void isAravisChanged();
    void isSpinnakerChanged();
    void needsDebayerChanged();
    void preprocessAvgMsChanged();
    void cameraFpsChanged();
    void poseAvgMsChanged();
    void poseFpsChanged();
    void poseBackendLabelChanged();
    void moveNetModelChanged();
    void poseKeypointsChanged();
    void poseEnabledChanged();
    void ballEnabledChanged();
    void perspectiveChanged();
    void isMirroredChanged();
    void roiChanged();
    void cropRoiChanged();
    void ballDetectedChanged();
    void ballPresencePercentChanged();
    void ballPresentChanged(bool present);
    // Struck-ball launch as a shot-arbiter candidate: absolute impact time on the
    // EventBuffer clock + confidence (< the arbiter self-commit floor — vision
    // only corroborates IMU/acoustic). Forwarded by CameraManager to the funnel.
    void ballLaunched(qint64 estImpactUs, float confidence);
    void ballAvgMsChanged();
    void frameSizeChanged();
    void isReplayingChanged();
    void deviceAliasChanged();
    void lastPreviewErrorChanged();

private slots:
    void onVideoFrame(const QVideoFrame &frame);
    void drainDisplayFrame();
    void drainRawFrame();
    void onVideoError(const QString &message);
    void onPreprocessStats(double avgMs);
    void onPoseStats(double avgMs, double fps);
    void onPoseBackendReady(const QString &label);
#ifdef HAVE_OPENCV
    void onPoseEstimated(const PoseResult &result);
    void onBallDetected(const BallDetection &result);
    // BallBaselineSnapshot carries a cv::Mat, so the slot itself is HAVE_OPENCV-
    // only even though the cache it fills (BallBaselineCache) is cv-free.
    void onBallBaselineReady(const BallBaselineSnapshot &snap);
#endif

private:
    void setupPipeline();
    void connectVideoInput();
#ifdef HAVE_PPCP_TRANSPORT
    // Resolves this instance's owning phone from its device id
    // ("ppcp:<peer_id>/<source_id>") and attaches VideoInputPpcp to its live
    // peer + session, if both are available.  A no-op for every other
    // backend, and safe to call whenever a start is about to be attempted:
    // re-resolved each time rather than cached, since a reconnected phone's
    // peer/session are a fresh ppcp_peer* and a fresh Session — the old ones
    // are not merely stale, they may already be destroyed.
    void ppcpAttachIfNeeded();
#endif
    void updateBufferDescriptor();
    void stampBufferDescriptorFromRaw(const RawVideoFrame &raw);
    // Refresh only the exposure fields of the already-stamped descriptor when the
    // exposure changes materially (auto-exposure). Metadata-only, main-thread, and
    // suppressed while a SwingWindow is live (a worker reads the descriptor then).
    void refreshExposure(double exposureUs, int exposureAutoMode);
    // frameTUs is the capture instant (nowMicros() sampled once on the capture
    // thread) that also travels to the pose/ball throttle — stamping the ring
    // entry with it keeps the ball track and the window frames on one clock at
    // offset zero.
    void publishFrameToBuffer(const QVideoFrame &frame, qint64 frameTUs);
    void publishRawFrameToBuffer(const RawVideoFrame &frame, qint64 frameTUs);

    pinpoint::EventBuffer *m_eventBuffer      = nullptr;
    pinpoint::SourceId     m_sourceId         = pinpoint::kInvalidSourceId;

    QThread               *m_captureThread   = nullptr;
    VideoInputBase        *m_videoInput       = nullptr;
    // Subscribed views (publish/subscribe — see addVideoSink). QPointer so a
    // view destroyed without unsubscribing is pruned on the next publish.
    QList<QPointer<QVideoSink>>     m_videoSinks;
    QList<QPointer<BayerVideoItem>> m_bayerItems;
    // QPointer (like the subscriber lists): the settings crop-UI VideoOutput
    // can be destroyed without setSettingsSink(null) reaching us first.
    QPointer<QVideoSink>   m_settingsSink;
    // Most recent frame delivered to the views — ground truth for
    // updateBufferDescriptor() (the subscriber list may be empty, so the
    // format can no longer be read back from "the" sink).
    QVideoFrame            m_lastDeliveredFrame;
    bool                   m_recording        = false;
    bool                   m_connecting       = false;
    bool                   m_previewing       = false;
    QString                m_lastPreviewError;

    // True for a Backend::Ppcp device — set once at construction, before
    // setupPipeline() runs, from the Device the constructor was given.  Drives
    // two things: setupPipeline() leaves m_videoInput on the GUI thread rather
    // than moving it to m_captureThread (VideoInputPpcp owns no socket/thread
    // of its own and is fed by whatever already-running PPCP pump drives it —
    // the dedicated capture thread exists for backends that block on real
    // I/O, which this one never does), and ppcpAttachIfNeeded() is only ever
    // worth calling when this is true.
    bool                   m_isPpcpBackend    = false;
#ifdef HAVE_PPCP_TRANSPORT
    PpcpHostService       *m_ppcpHostService  = nullptr;
#endif

    // Guarded setter for m_connecting — emits isConnectingChanged only on change.
    void setConnecting(bool on);
    std::atomic<bool>      m_previewOnly{false}; // true = camera running but pipeline suppressed
    QString                m_deviceId;
    QString                m_deviceDescription;
    QString                m_deviceSerialNumber;
    QString                m_deviceAlias;

    // Display-path throttle: at most one frame is ever queued to the main thread
    // at a time (shared between the QVideoFrame and RawVideoFrame paths, which are
    // mutually exclusive per camera type).
    std::atomic<bool>      m_displayFramePending{false};

    // Latched after the first time publishFrameToBuffer() drops a frame that won't
    // fit the ring slot, so the error is logged once rather than every frame (which
    // would flood the bounded log at the capture rate and hide everything else).
    std::atomic<bool>      m_publishDropLogged{false};

    // Set once the source descriptor's pixel format + resolution have been stamped
    // from the first real delivered frame (main-thread only — see drainDisplayFrame).
    bool                   m_bufferDescriptorStamped = false;
    // Dims/pattern last stamped via the raw Bayer path, so a changed ROI re-stamps
    // (main-thread only — see drainRawFrame).
    int                    m_stampedRawWidth  = 0;
    int                    m_stampedRawHeight = 0;
    RawVideoFrame::BayerPattern m_stampedRawPattern = RawVideoFrame::BayerPattern::RG;
    // Cache of the last descriptor sent to updateSourceFormat(), so refreshExposure()
    // can re-send it with only the exposure fields changed (no stride re-probe).
    pinpoint::FormatDescriptor m_stampedDescriptor;

    QMutex                 m_latestFrameMutex;
    QVideoFrame            m_latestDisplayFrame;

    QMutex                 m_latestRawFrameMutex;
    RawVideoFrame          m_latestRawFrame;

    QThread               *m_preprocessThread = nullptr;
    VideoPreprocessorBase *m_preprocessor     = nullptr;
    double                 m_preprocessAvgMs  = 0.0;

#ifdef HAVE_OPENCV
    QThread           *m_poseThread    = nullptr;
    PoseEstimatorBase *m_poseEstimator = nullptr;
    FrameThrottle     *m_frameThrottle = nullptr;
    QThread           *m_ballThread    = nullptr;
    BallDetector      *m_ballDetector  = nullptr;
#endif
    bool   m_ballDetected = false;
    double m_ballX        = 0.0;
    double m_ballY        = 0.0;
    double m_ballRadius   = 0.0;

    static constexpr int kBallWindowSize      = 50;
    static constexpr double kBallPresentThreshold = 30.0;
    std::deque<bool> m_ballWindow;
    int              m_ballPresentCount     = 0;
    double           m_ballPresencePercent  = 0.0;
    bool             m_ballPresent          = false;
    double           m_ballAvgMs            = 0.0;

    // v3.4 ball-anchor accumulator (design §9) — see ballSamples()/ballLaunchInfo().
    static constexpr qint64 kBallAccumWindowUs = 6'000'000;   // > the 5 s SwingWindow ring
    std::deque<BallAccumSample> m_ballAccum;
    qint64 m_ballLaunchTUs = -1;
    double m_ballLaunchX   = 0.0;
    double m_ballLaunchY   = 0.0;
    void trimBallAccum();

    // See ballBaseline(). Cleared to {} (not just left stale) on relearn/ROI
    // change so a shot fired mid-reseed exports no baseline rather than a scene
    // that no longer matches the live ROI.
    BallBaselineCache m_ballBaseline;
    TingPlayer      *m_tingPlayer           = nullptr;
    bool             m_replaying            = false;
    // Capture-rate FPS: counted on the capture thread, sampled on a timer.
    std::atomic<int>   m_frameCaptureCount{0};
    QTimer            *m_fpsSampleTimer    = nullptr;
    QElapsedTimer      m_fpsSampleElapsed;
    double             m_cameraFps         = 0.0;
    double             m_poseAvgMs          = 0.0;
    double             m_poseFps            = 0.0;
    QString            m_poseBackendLabel;
    int                m_moveNetModel       = 0;
    QVariantList       m_poseKeypoints;
    bool               m_poseEnabled        = true;
    bool               m_ballEnabled        = true;
    int                m_perspective        = 0;
    bool               m_isMirrored         = false;
    QRectF             m_roi;
    QRectF             m_cropRoi;
    int                m_frameWidth  = 0;
    int                m_frameHeight = 0;
    double             m_configuredFps = 0.0;

    // Crop state frozen at construction ("applies at connect" — capture
    // instances are recreated per connect). Read on the capture thread and
    // never mutated afterwards, so no locking is needed. m_cropRoi above
    // remains the live UI/persistence value and may diverge until the next
    // connect.
    QRectF             m_activeCropRoi;          // empty when no crop configured
    bool               m_cropEnabled        = false; // crop set AND buffer-backed instance
    int                m_sensorWidth        = 0;     // full-sensor dims used for slot sizing
    int                m_sensorHeight       = 0;
    int                m_expectedCropWidth  = 0;     // ceil16(crop x sensor); 0 = no crop
    int                m_expectedCropHeight = 0;
};
