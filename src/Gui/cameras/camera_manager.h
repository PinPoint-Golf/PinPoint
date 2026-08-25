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

#include <QList>
#include <QObject>
#include <QRectF>
#include <QVariantList>
#include <vector>

#include "device_enumerator.h"
#include "app_settings.h"
#include "../Buffer/types.h"
#include "../Video/camera_capabilities.h"

namespace pinpoint { class EventBuffer; }
class CameraInstance;
class ShotProcessor;
#ifdef HAVE_PPCP_TRANSPORT
class PpcpHostService;
#endif

class CameraManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList cameraList  READ cameraList  NOTIFY cameraListChanged)
    Q_PROPERTY(QVariantList instances   READ instances   NOTIFY instancesChanged)
    Q_PROPERTY(bool isRecording         READ isRecording NOTIFY isRecordingChanged)
    Q_PROPERTY(bool anySelected         READ anySelected NOTIFY instancesChanged)
    // True while ANY live camera instance has a connect attempt in flight —
    // drives the aggregate Connect button's "connecting" animation.
    Q_PROPERTY(bool anyConnecting       READ anyConnecting NOTIFY anyConnectingChanged)
    Q_PROPERTY(QString bufferState      READ bufferState NOTIFY bufferStateChanged)
    // Per-session camera enablement (cameraKey list). Seeded from
    // AppSettings::cameraExcluded at startup but NEVER written back — global
    // enablement is owned by Settings. Shared by every toolbar camera panel
    // and the per-screen video tile rows (all screens stay alive in the
    // StackLayout, so this state cannot live in a panel instance).
    Q_PROPERTY(QStringList sessionCameraExcluded READ sessionCameraExcluded NOTIFY sessionCameraExcludedChanged)
    // All-cameras live pose-estimation toggle (session-wide, not persisted).
    Q_PROPERTY(bool livePoseEnabled READ livePoseEnabled WRITE setLivePoseEnabled NOTIFY livePoseEnabledChanged)

public:
    explicit CameraManager(pinpoint::EventBuffer *buffer = nullptr,
                           AppSettings *appSettings = nullptr,
                           QObject *parent = nullptr);
    ~CameraManager() override;

    QVariantList cameraList() const;
    QVariantList instances()  const;
    bool isRecording()        const;
    bool anySelected()        const;
    bool anyConnecting()      const;
    QString bufferState()     const;
    // The user's capture intent (set by startCapture/stopCapture). Unlike
    // bufferState this stays stable across a session — it doesn't toggle when
    // the buffer pauses mid-shot for SwingWindow processing — so it's the right
    // signal for gating session-lifetime work like acoustic shot detection.
    bool captureIntent()      const { return m_captureUserEnabled; }
    QStringList sessionCameraExcluded() const;
    bool livePoseEnabled()    const;
    void setLivePoseEnabled(bool on);

    // Returns the live CameraInstance for a given device ID, or nullptr when
    // the device is enumerated but not selected.  Mirrors ImuManager::instanceFor().
    CameraInstance *instanceFor(const QString &deviceId) const;

    // Snapshot of live per-device stats for monitoring purposes.
    // Avoids exposing CameraInstance to callers that only need metrics.
    struct CameraDeviceStats {
        pinpoint::SourceId sourceId  = pinpoint::kInvalidSourceId;
        double             fps       = 0.0;
        int                width     = 0;
        int                height    = 0;
        bool               recording = false;
        QRectF             cropRoi;
    };
    CameraDeviceStats liveDeviceStats(const QString &deviceId) const;

    // The shot processor owns the SwingWindow lifecycle. CameraManager calls
    // finishNowBlocking() on it before any source deregistration (camera
    // deselect, destruction) — the teardown stop-barrier.
    void setShotProcessor(ShotProcessor *p) { m_shotProcessor = p; }

#ifdef HAVE_PPCP_TRANSPORT
    // A PPCP camera (Backend::Ppcp) is built with a peer never attached — see
    // createPreviewInstance()/createController() — so every CameraInstance
    // this manager builds needs a way to ask its owning phone's peer.  Set
    // once from main.cpp, forwarded to each CameraInstance at construction.
    void setPpcpHostService(PpcpHostService *svc) { m_ppcpHostService = svc; }
#endif

    // Selected cameras with a live controller — the shot processor builds its
    // replay tracks and export job from these.
    std::vector<CameraInstance *> liveCameraInstances() const;

    Q_INVOKABLE void setSelected(int index, bool selected);
    Q_INVOKABLE void startAll();
    Q_INVOKABLE void stopAll();
    // End-of-session device release: stops recording, then deselects every
    // connected camera through the normal setSelected teardown.
    Q_INVOKABLE void disconnectAll();
    Q_INVOKABLE void setCameraAlias(const QString &key, const QString &alias);
    Q_INVOKABLE void pauseBuffer();
    Q_INVOKABLE void resumeBuffer();
    Q_INVOKABLE void enumerate();

    // Session-global capture intent (toolbar Capture/Stop). The EventBuffer
    // captures iff the user enabled capture (and sources exist) — device
    // connect/disconnect, start/stop and ball detection never change the net
    // buffer state.
    Q_INVOKABLE void startCapture();   // intent = on,  resume the buffer
    Q_INVOKABLE void stopCapture();    // intent = off, pause the buffer

public slots:
    // Re-applies the user capture intent to the buffer. Connected to
    // ImuManager::bufferStateChanged in main() so IMU register/deregister
    // (and the buffer's first-source auto-resume) can't leave the buffer in a
    // state the user didn't ask for.
    void applyCaptureIntent();

    // Sets the perspective on one camera. Any number of cameras may share a
    // perspective (e.g. two face-on cameras in one session).
    Q_INVOKABLE void setPerspective(QObject *controller, int perspective);
    Q_INVOKABLE void setIsMirrored(QObject *controller, bool mirrored);

    // Sets the hitting-area ROI on one camera and persists it per cameraKey.
    // Restored on connect when the camera is fixed in place (a moved camera
    // invalidates the stored area). An empty rect clears both. Changing the
    // ROI drops any live calibration profile (hard invalidation, design §6).
    Q_INVOKABLE void setBallRoi(QObject *controller, QRectF roi);
    Q_INVOKABLE void clearBallRoi(QObject *controller);
    // v2: re-learn the empty-mat baseline for this camera's temporal ball
    // detector (Option A). Call with the mat empty.
    Q_INVOKABLE void relearnBallBaseline(QObject *controller);

    Q_INVOKABLE void setExcluded(int index, bool excluded);
    Q_INVOKABLE void setSessionCameraEnabled(const QString &cameraKey, bool on);
    Q_INVOKABLE void setTargetFps(int index, double fps);
    Q_INVOKABLE void setTriggerMode(int index, const QString &mode);

    // Creates a lightweight preview-only CameraInstance (no event buffer, no pose
    // pipeline) for use in the settings panel crop UI.  The returned object is owned
    // by this CameraManager and must be released via destroyPreviewInstance() when done.
    Q_INVOKABLE QObject *createPreviewInstance(int index);
    Q_INVOKABLE void     destroyPreviewInstance(QObject *ctrl);

signals:
    void cameraListChanged();
    void instancesChanged();
    void isRecordingChanged();
    void anyConnectingChanged();
    // Struck-ball launch from any camera's temporal detector, forwarded to the
    // shot-arbiter funnel in main.cpp (absolute impact time + confidence).
    void ballLaunched(qint64 estImpactUs, float confidence);
    void bufferStateChanged();
    void captureIntentChanged();
    void sessionCameraExcludedChanged();
    void livePoseEnabledChanged();

private:
    struct CameraEntry {
        Device device;
        bool selected = false;
        CameraInstance *controller = nullptr;
        bool excluded = false;
        double targetFps = 0.0;
        QString triggerMode = QStringLiteral("freerun");
    };

    // Stable per-device key for settings storage.
    // Format: "ModelName|SerialNumber" so keys are human-readable in QSettings and
    // immune to cross-manufacturer serial-number collisions.
    // Falls back to "ModelName|DeviceId" when no serial is available.
    static QString cameraKey(const CameraEntry &cam)
    {
        const QString model = cam.device.description;
        const QString sn    = cam.device.capabilities.serialNumber;
        const QString id    = sn.isEmpty() ? cam.device.id : sn;
        return model + QStringLiteral("|") + id;
    }

    static QString interfaceString(CameraCapabilities::Interface iface)
    {
        switch (iface) {
        case CameraCapabilities::Interface::USB2:  return QStringLiteral("USB2");
        case CameraCapabilities::Interface::USB3:  return QStringLiteral("USB3");
        case CameraCapabilities::Interface::GigE:
        case CameraCapabilities::Interface::GigE5:
        case CameraCapabilities::Interface::GigE10: return QStringLiteral("GigE");
        default:                                    return QStringLiteral("Unknown");
        }
    }

    QList<CameraEntry>                   m_cameras;
    QStringList                          m_sessionExcluded;
    bool                                 m_livePoseEnabled  = true;
    bool                                 m_recording        = false;
    bool                                 m_captureUserEnabled = false;  // toolbar Capture/Stop
    pinpoint::EventBuffer               *m_eventBuffer      = nullptr;
    AppSettings                         *m_appSettings      = nullptr;
    ShotProcessor                       *m_shotProcessor    = nullptr;
#ifdef HAVE_PPCP_TRANSPORT
    PpcpHostService                     *m_ppcpHostService  = nullptr;
#endif

    CameraInstance *createController(const Device &device);
};
