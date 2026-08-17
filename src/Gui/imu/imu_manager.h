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
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QVariantList>

#include "app_settings.h"
#include "device_enumerator.h"
#include "imu_device.h"
#include "types.h"

namespace pinpoint { class EventBuffer; }

class ShotProcessor;

// Manages N ImuDeviceBase instances (Witmotion ImuInstance or HackMotion
// HmInstance), one per user-selected IMU device.
// Mirrors the CameraManager pattern: imuList / instances / setSelected().
//
// Device discovery is the authority of DeviceEnumerator — imuList() and
// imuDeviceList() always read from it so they stay current even before
// deviceAdded signals fire. m_selected is a map of device-id → ImuEntry
// tracking only selection state and live instances.
class ImuManager : public QObject
{
    Q_OBJECT

    // ── Per-device chip list (includes runtime connected/connecting state) ──
    Q_PROPERTY(QVariantList imuList     READ imuList     NOTIFY imuListChanged)
    // ── Active instances exposed as QObject* for QML Repeater ─────────────
    Q_PROPERTY(QVariantList instances   READ instances   NOTIFY instancesChanged)
    Q_PROPERTY(bool         anySelected READ anySelected NOTIFY instancesChanged)
    // ── Enumeration / capability list for the Settings ImusPanel ──────────
    Q_PROPERTY(int          imuEnumeratedCount READ imuEnumeratedCount NOTIFY imuEnumeratedCountChanged)
    Q_PROPERTY(QVariantList imuDeviceList      READ imuDeviceList      NOTIFY imuDeviceListChanged)
    // ── Aggregate state (any connected / count) ───────────────────────────
    Q_PROPERTY(bool imuConnected READ imuConnected NOTIFY instancesChanged)
    Q_PROPERTY(int  imuCount     READ imuCount     NOTIFY instancesChanged)
    // True while ANY live IMU instance has a connect attempt in flight (busy:
    // from start() through retry/backoff/watchdog until success or final
    // failure). Drives the aggregate Connect button's "connecting" animation.
    Q_PROPERTY(bool anyConnecting READ anyConnecting NOTIFY anyConnectingChanged)
    // Lowest battery % across all *connected* IMUs (−1 when none report a
    // level). Drives the toolbar IMU-pill low-battery warning. Notifies on live
    // battery updates as well as connect/disconnect (both change the minimum).
    Q_PROPERTY(int  lowBatteryPercent READ lowBatteryPercent NOTIFY batteryChanged)
    // ── Per-session IMU enablement (device ids excluded this session) ──────
    // Mirrors CameraManager::sessionCameraExcluded: manager-owned so the start
    // wizard, every toolbar IMU panel and the device rows share ONE list.
    // Seeded from AppSettings::imuExcluded at startup (and re-seeded by the
    // wizard on open); NEVER written back — global enablement is owned by the
    // Settings screen.
    Q_PROPERTY(QStringList sessionImuExcluded READ sessionImuExcluded NOTIFY sessionImuExcludedChanged)
    // Last BLE discovery error (e.g. "Bluetooth is powered off"), or "" when
    // discovery is healthy. The IMU UI shows this when no devices are enumerated
    // so an empty list is distinguishable from "discovery couldn't run". Cleared
    // when a device is found or a fresh scan starts.
    Q_PROPERTY(QString imuScanError READ imuScanError NOTIFY imuScanErrorChanged)

public:
    explicit ImuManager(pinpoint::EventBuffer *buffer = nullptr,
                        AppSettings *appSettings = nullptr,
                        QObject *parent = nullptr);
    ~ImuManager() override;

    // Both list accessors read from DeviceEnumerator directly (the old pattern)
    // so they are always current regardless of deviceAdded signal timing.
    QVariantList imuList()            const;
    QVariantList imuDeviceList()      const;
    QVariantList instances()          const;
    bool         anySelected()        const;
    int          imuEnumeratedCount() const;
    bool         imuConnected()       const;
    int          imuCount()           const;
    bool         anyConnecting()      const;
    int          lowBatteryPercent()  const;

    // Teardown stop-barrier (same contract as CameraManager): deregistering an
    // IMU source while a SwingWindow is live frees ring memory under the shot
    // workers. setSelected(deselect) and the destructor call
    // finishNowBlocking() first. Set from main.cpp.
    void setShotProcessor(ShotProcessor *p) { m_shotProcessor = p; }

    // Toggle selection (= connect/disconnect + EventBuffer register/deregister).
    // index is the position in DeviceEnumerator::devices(DeviceType::Imu).
    Q_INVOKABLE void setSelected(int index, bool selected);

    // End-of-session device release: disconnects every connected IMU through
    // the normal setSelected teardown.
    Q_INVOKABLE void disconnectAll();

    QStringList sessionImuExcluded() const;
    QString     imuScanError() const { return m_imuScanError; }

    // Per-session enablement toggle. Disabling a selected/connected device also
    // disconnects it; enabling never auto-connects (Connect does that).
    Q_INVOKABLE void setSessionImuEnabled(const QString &deviceId, bool on);

    // Trigger a new BLE scan to find devices.
    Q_INVOKABLE void rescanImu();

    // Persist a user-visible alias for a device. key = description|id.
    // Pass empty alias to revert to default (device description).
    Q_INVOKABLE void setImuAlias(const QString &key, const QString &alias);

    // Select the local orientation-fusion algorithm ("Madgwick" / "ESKF") for all
    // IMUs: persists to AppSettings and pushes the change to every live instance.
    Q_INVOKABLE void setOrientationFilter(const QString &name);

    // Returns the live instance's QObject* for deviceId (Witmotion or
    // HackMotion — see ImuEntry), or nullptr if not selected.
    Q_INVOKABLE QObject *instanceFor(const QString &deviceId) const;

    // Snapshot of live per-device stats for monitoring purposes.
    // Avoids exposing ImuDeviceBase to callers that only need metrics.
    struct ImuDeviceStats {
        // ⚠ PLURAL — see ImuDeviceBase::sourceIds(). A Witmotion contributes at
        // most one id; a HackMotion contributes two once Phase B registers them
        // (zero in Phase A, since HmInstance::sourceIds() is empty on purpose —
        // see imu_device.h). Nothing left here reads a single scalar id, so
        // there is no separate `sourceId` field to keep in sync.
        std::vector<pinpoint::SourceId> sourceIds;
        double             dataRateHz     = 0.0;
        int                batteryPercent = -1;
        int                gimbalDropCount = 0;
        bool               connected      = false;
        bool               busy           = false;
        // ⚠ Explicit, not inferred from sourceIds being non-empty. A Phase A
        // HackMotion registers zero sources even while connected, so "no
        // sources" no longer means "never selected" the way it did when every
        // selected device registered exactly one. A caller distinguishing
        // "idle" (never selected) from "disconnected" (selected, link down)
        // needs this bit directly.
        bool               selected       = false;
    };
    ImuDeviceStats liveDeviceStats(const QString &deviceId) const;

    // Aggregate save — writes one log file per active instance.
    Q_INVOKABLE QString saveLog();

    // Zero orientation on all connected instances.
    Q_INVOKABLE void zeroAll();

signals:
    void imuListChanged();
    void instancesChanged();
    void anyConnectingChanged();
    void imuEnumeratedCountChanged();
    void imuDeviceListChanged();
    void sessionImuExcludedChanged();
    void imuScanErrorChanged();
    // Aggregate battery state changed — a connected IMU reported a new level, or
    // the connected set changed. Backs the lowBatteryPercent property.
    void batteryChanged();
    // EventBuffer state may have changed (source register/deregister can pause
    // or auto-resume the shared buffer). Forwarded to
    // CameraManager::applyCaptureIntent in main() — the QML-facing buffer state
    // lives on CameraManager.
    void bufferStateChanged();
    // Aggregated log entries forwarded from all active instances.
    void logEntryAdded(const QString &entry);

    // IMU impact auto-trigger (shot detection P1) — re-emitted from every
    // live ImuInstance; main.cpp routes it to ShotController::triggerShot
    // behind the autoDetectSwing setting.
    void impactDetected(qint64 estImpactUs, float confidence);

private:
    struct ImuEntry {
        bool           selected = false;
        // ⚠ The device-kind-agnostic base (imu_device.h), not ImuInstance —
        // this map holds either a Witmotion ImuInstance or a HackMotion
        // HmInstance, and every site that touches it only needs what both
        // answer. Callers that need the concrete Witmotion API (shot_processor,
        // live_wrist_angles) qobject_cast<ImuInstance*> over instances().
        ImuDeviceBase *instance = nullptr;
    };

    ImuDeviceBase *createInstance(const Device &device);
    // swingDetectionSensitivity ("Low"/"Medium"/"High") → detector threshold scale.
    static float impactScaleFor(const QString &sensitivity);

    // True if the device appeared in the most recently completed BLE scan, OR is
    // currently selected (a connected device stops advertising, so it would
    // otherwise wrongly vanish). Drives the "present" flag in the chip lists —
    // absent devices are hidden in the chip rows and dimmed in the Settings list.
    bool isImuPresent(const Device &dev) const;

    // The shared IMU I/O thread (imu_io_thread_impl.md): hosts every device's
    // BLE driver + ImuIoWorker, so packet parse / fusion / impact detection /
    // ring writes never touch the GUI thread. One thread for all devices —
    // Qt BLE is fully async and 3 × 200 Hz is nothing for one event loop.
    // Joined in the destructor AFTER the instances are gone.
    QThread m_ioThread;

    pinpoint::EventBuffer      *m_eventBuffer  = nullptr;
    AppSettings                *m_appSettings  = nullptr;
    ShotProcessor              *m_shotProcessor = nullptr;
    QMap<QString, ImuEntry>     m_selected;    // keyed by device id

    // Device ids whose previous instance is still pending deletion (deselect
    // schedules deleteLater via singleShot(0)). A re-select for such an id is
    // ignored until teardown settles, so a fast select→deselect→select can't
    // create two instances / overlap connects on the same HCI adapter.
    QSet<QString>               m_pendingDelete;

    // Per-session exclusion (device ids). Seeded from AppSettings::imuExcluded;
    // m_lastGlobalExcluded snapshots the global list so mid-session Settings
    // changes can be diffed into the session list (CameraManager::setExcluded
    // does the same sync for cameras).
    QStringList m_sessionExcluded;
    QStringList m_lastGlobalExcluded;

    // Last BLE discovery error surfaced to QML (empty = healthy). Helper keeps the
    // set-and-notify in one place.
    QString m_imuScanError;
    void setImuScanError(const QString &msg);
};
